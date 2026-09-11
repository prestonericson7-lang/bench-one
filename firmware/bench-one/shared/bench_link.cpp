/* ===========================================================================================
 *  bench_link.cpp -- one framed UART link
 * ===========================================================================================
 */

#include "bench_link.h"

BenchLink::BenchLink()
    : mPort(0), mName("?"), mPeer(BENCH_NODE_UNKNOWN),
      mReqSeq(0), mEvtSeq(0),
      mPending(false), mPendingSeq(0), mPendingChan(0), mPendingCmd(0),
      mPendingStartMs(0), mPendingTimeoutMs(0),
      mPeerSeen(false), mLastRxMs(0), mLastEventSeq(0), mEventSeqValid(false),
      mCtx(0), mOnFrame(0), mOnTimeout(0)
{
    memset(&mStats, 0, sizeof(mStats));
    iop_parser_init(&mParser);
    iop_nack_limiter_init(&mNackLimiter);
}

void BenchLink::begin(HardwareSerial *port, const char *name, uint8_t peer, uint32_t baud,
                      uint8_t *rxBuf, size_t rxBufLen)
{
    mPort = port;
    mName = name;
    mPeer = peer;

    mPort->begin(baud);

    /* The receive ring is what buys time when loop() is busy. Serial receive on a Teensy 4 is
     * ISR-driven, so a blocked loop() delays the PARSER, not RECEPTION -- the bytes keep landing
     * in the ring regardless. The ceiling is therefore the ring's depth, not the inter-byte
     * timeout, and a deeper ring converts a display refresh or a slow fabric transaction from a
     * lost frame into a slightly late one.
     *
     * This is a real correction from this project's own history: the original worry was that a
     * 28 ms blocking refresh would break the 50 ms inter-byte timeout. It cannot, for the reason
     * above. What it CAN do is land inside a latency measurement and get recorded as latency,
     * which is a different failure and produces a plausible wrong number rather than an error.
     *
     * The buffer is passed IN rather than allocated here. An earlier draft kept a static pool
     * inside this function handed out by a counter, which is hidden global state that silently
     * stops working on the fifth link and cannot be sized per link. The caller knows how much
     * RAM it has and which link carries the heaviest traffic; this class does not. */
#if defined(__IMXRT1062__)
    /* addMemoryForRead lives on HardwareSerialIMXRT, not on the HardwareSerial base this class
     * stores. Every Serial1..Serial8 object on a Teensy 4 IS a HardwareSerialIMXRT, so the
     * downcast is safe here and only here -- which is why it sits inside the __IMXRT1062__
     * guard rather than in the portable interface. The compiler found this; the base-class
     * pointer looked right and is not. */
    if (rxBuf && rxBufLen > 0u) {
        static_cast<HardwareSerialIMXRT *>(mPort)->addMemoryForRead(rxBuf, rxBufLen);
    }
#else
    (void)rxBuf; (void)rxBufLen;
#endif

    iop_parser_init(&mParser);
    iop_nack_limiter_init(&mNackLimiter);
}

void BenchLink::setHandlers(void *ctx, BenchLinkFrameFn onFrame, BenchLinkTimeoutFn onTimeout)
{
    mCtx = ctx;
    mOnFrame = onFrame;
    mOnTimeout = onTimeout;
}

void BenchLink::resetStats()
{
    memset(&mStats, 0, sizeof(mStats));
}

void BenchLink::poll()
{
    if (!mPort) { return; }

    uint32_t now = millis();
    IopFrame frame;

    /* Drain the whole ring before doing anything else. Reading a single byte per pass would fall
     * behind the wire at 921600 and overflow the ring -- one byte is 10.9 us, and a loop()
     * iteration that does anything at all is longer than that. */
    while (mPort->available() > 0) {
        uint8_t b = (uint8_t)mPort->read();
        IopEvent ev = iop_parser_push(&mParser, b, now, &frame);
        if (ev == IOP_EV_FRAME) {
            mStats.frames_rx++;
            mLastRxMs = now;
            mPeerSeen = true;
            handleFrame(&frame);
        }
    }

    /* THE TIMEOUT IS EVALUATED ONLY WITH AN EMPTY QUEUE. If bytes are still buffered, the sender
     * is not stalled -- this node is merely behind, and abandoning a partial frame here discards
     * data that arrived perfectly well. This ordering is load-bearing, not incidental. */
    if (mPort->available() == 0) {
        if (iop_parser_tick(&mParser, now, &frame) == IOP_EV_FRAME) {
            mStats.frames_rx++;
            mLastRxMs = now;
            mPeerSeen = true;
            handleFrame(&frame);
        }
    }

    /* Request timeout. Fires once, clears the pending slot, and tells the owner what it was
     * waiting for -- so the message can name the operation instead of just reporting that time
     * passed. */
    if (mPending && (now - mPendingStartMs) >= mPendingTimeoutMs) {
        uint8_t chan = mPendingChan, cmd = mPendingCmd, seq = mPendingSeq;
        mPending = false;
        mStats.timeouts++;
        if (mOnTimeout) { mOnTimeout(mCtx, chan, cmd, seq); }
    }
}

void BenchLink::handleFrame(const IopFrame *f)
{
    /* UNSOLICITED FIRST, always, and before the stale check.
     *
     * An event answers no request, so the stale-response test below would discard every one of
     * them as unexpected. The class bit makes the two categories impossible to confuse, which is
     * exactly why v2 split the sequence byte instead of reserving a value. */
    if (IOP_SEQ_IS_UNSOLICITED(f->seq)) {
        mStats.events++;

        /* A HELLO means the far end RESTARTED, so its event counter legitimately went back to
         * the start of the class. Counting that as loss reports a large fake number the first
         * time a node is reflashed while this one keeps running, and a counter that cries wolf
         * once gets ignored forever after. Re-anchor instead. */
        bool isHello = (f->cmd == BENCH_CMD_WRK_HELLO ||
                        f->cmd == BENCH_CMD_ORC_HELLO ||
                        f->cmd == BENCH_CMD_HMI_HELLO ||
                        f->cmd == IOP_CMD_RADIO_READY);

        if (isHello) {
            anchorEvents(f->seq);
        } else if (mEventSeqValid) {
            uint8_t expected = iop_next_event_seq(mLastEventSeq);
            if (f->seq != expected) {
                /* The gap is computed on the 7-bit counter only. Including the class bit in the
                 * arithmetic would make a wrap look like 128 lost events. */
                uint8_t missed = (uint8_t)((f->seq - expected) & IOP_SEQ_COUNTER_MASK);
                mStats.events_missed += missed;
            }
            mLastEventSeq = f->seq;
        } else {
            anchorEvents(f->seq);
        }

        if (mOnFrame) { mOnFrame(mCtx, f, false); }
        return;
    }

    /* From here it claims to be a response. */
    if (f->cmd == IOP_CMD_NACK) { mStats.nacks++; }

    if (!mPending || f->seq != mPendingSeq) {
        /* A leftover: the answer to a request that already timed out. Acting on it would
         * attribute an old answer to a new question, and every response after that is off by
         * one. Counted, so the fault is visible rather than merely felt. */
        mStats.stale++;
        return;
    }

    mPending = false;
    mStats.responses++;
    if (mOnFrame) { mOnFrame(mCtx, f, true); }
}

uint8_t BenchLink::sendFrame(uint8_t seq, uint8_t flags, uint8_t chan, uint8_t cmd,
                             const uint8_t *payload, uint16_t len)
{
    if (!mPort) { return BENCH_ST_HW_FAULT; }

    uint16_t n = iop_build_frame(mTxBuf, sizeof(mTxBuf), seq, flags, chan, cmd, payload, len);
    if (n == 0u) {
        /* The builder refused. Almost always an oversize payload. It is counted and reported as
         * OVERFLOW rather than being truncated to fit: a truncated frame is a valid frame
         * carrying incomplete data, which the far end has no way to detect. */
        mStats.tx_overflow++;
        return BENCH_ST_OVERFLOW;
    }

    mPort->write(mTxBuf, n);
    mStats.frames_tx++;
    return BENCH_ST_OK;
}

uint8_t BenchLink::request(uint8_t chan, uint8_t cmd, const uint8_t *payload, uint16_t len,
                           uint32_t timeout_ms)
{
    /* One outstanding request per link, and a second one is REFUSED rather than queued.
     *
     * A queue here would be the wrong kind of helpful. It hides backpressure from the caller,
     * and backpressure is information: it is how the Linux box learns it is asking faster than
     * the fabric can answer. Returning BUSY makes that visible at the point where something can
     * actually be done about it. */
    if (mPending) {
        mStats.busy_rejects++;
        return BENCH_ST_BUSY;
    }

    mReqSeq = iop_next_request_seq(mReqSeq);

    uint8_t st = sendFrame(mReqSeq, IOP_FLAG_NONE, chan, cmd, payload, len);
    if (st != BENCH_ST_OK) { return st; }

    mPending           = true;
    mPendingSeq        = mReqSeq;
    mPendingChan       = chan;
    mPendingCmd        = cmd;
    mPendingStartMs    = millis();
    mPendingTimeoutMs  = timeout_ms;
    mStats.requests++;
    return BENCH_ST_OK;
}

uint8_t BenchLink::respond(uint8_t seq, uint8_t chan, uint8_t cmd,
                           const uint8_t *payload, uint16_t len)
{
    /* Echo the request's SEQ exactly. That byte is the only thing tying this reply to its
     * question; generating a fresh one would make every response look stale to the far end. */
    return sendFrame(seq, IOP_FLAG_NONE, chan, cmd, payload, len);
}

uint8_t BenchLink::event(uint8_t chan, uint8_t cmd, const uint8_t *payload, uint16_t len)
{
    mEvtSeq = iop_next_event_seq(mEvtSeq);
    return sendFrame(mEvtSeq, IOP_FLAG_NONE, chan, cmd, payload, len);
}

uint8_t BenchLink::nack(uint8_t seq, uint8_t chan, uint8_t cmd, uint8_t error)
{
    /* Rate-limited on purpose. A peer emitting garbage -- wrong baud, half-flashed, a floating
     * TX line -- would otherwise be answered with one NACK per garbage frame, doubling the
     * traffic on a link that is already failing and potentially wedging both ends in a NACK
     * storm. The limiter counts what it suppressed, and that count is reported rather than
     * hidden, because "we suppressed 4,000 NACKs" is the diagnosis. */
    if (!iop_nack_should_send(&mNackLimiter, millis())) {
        return BENCH_ST_BUSY;
    }

    uint8_t payload[3] = { chan, cmd, error };
    return sendFrame(seq, IOP_FLAG_NONE, IOP_CHAN_TRANSPORT, IOP_CMD_NACK, payload, 3);
}
