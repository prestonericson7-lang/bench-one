/* ===========================================================================================
 *  bench_link.h -- one framed UART link, reusable on every node
 * ===========================================================================================
 *
 *  WHY THIS EXISTS AS A SEPARATE MODULE
 *  ------------------------------------
 *  The tested master controller manages exactly one link, the ESP32-S3 radio, and its link state
 *  is woven through the sketch as globals: one parser, one pending request, one set of counters.
 *  That was right when there was one link. BENCH ONE has four.
 *
 *  The wrong way to get from one to four is to copy that code three times. Every fix would then
 *  need applying four times, and the first one anybody forgets becomes a link that behaves
 *  subtly differently from its neighbours -- which is precisely the class of failure this
 *  project's bring-up log is a list of.
 *
 *  So: one class, instantiated per link. The tested radio code is NOT ported into it and is not
 *  touched at all. It keeps working exactly as measured. This class serves the three NEW links,
 *  and the same object serves the worker and display nodes, where it is the only link there is.
 *
 *
 *  WHAT IT ENFORCES
 *  ----------------
 *  ONE OUTSTANDING REQUEST PER LINK. The protocol's SEQ byte supports pipelining; the endpoints
 *  do not implement it, and pretending otherwise would let two requests share one sequence
 *  number. Attempting a second request while one is in flight returns BUSY rather than queueing,
 *  because a queue would hide the backpressure that a caller needs to see.
 *
 *  STALE RESPONSES ARE DROPPED AND COUNTED. A reply whose SEQ does not match the request in
 *  flight is the answer to something that already timed out. Acting on it would attribute an old
 *  answer to a new question, and from then on every response is off by one -- a fault that gets
 *  worse the longer it runs and never announces itself.
 *
 *  EVENT GAPS ARE VISIBLE. Unsolicited frames carry their own rolling counter. A missing
 *  announcement is detected and reported rather than silently changing this node's model of the
 *  world. A HELLO re-anchors the counter instead of counting as loss, because a node that
 *  rebooted legitimately restarted its counter and reporting that as "127 events missed" is the
 *  false alarm that teaches people to ignore a counter.
 *
 *  THE TIMEOUT IS EVALUATED ONLY WHEN THE RECEIVE QUEUE IS EMPTY. Learned on hardware in this
 *  project: checking it while bytes are still buffered makes a slow reader look like a stalled
 *  sender, and the parser discards frames that arrived perfectly well.
 * ===========================================================================================
 */

#ifndef BENCH_LINK_H
#define BENCH_LINK_H

#include <Arduino.h>
#include <stdint.h>

#include "interop_protocol.h"
#include "bench_protocol.h"

/* A complete, CRC-valid frame arrived. `solicited` is true if it answered our request. */
typedef void (*BenchLinkFrameFn)(void *ctx, const IopFrame *frame, bool solicited);

/* A request timed out. cmd is what was asked, so the handler has enough context to say what
 * actually failed rather than just that something did. */
typedef void (*BenchLinkTimeoutFn)(void *ctx, uint8_t chan, uint8_t cmd, uint8_t seq);

typedef struct {
    uint32_t frames_tx;
    uint32_t frames_rx;
    uint32_t requests;
    uint32_t responses;
    uint32_t events;
    uint32_t timeouts;
    uint32_t stale;          /* a reply to something that already timed out                    */
    uint32_t nacks;
    uint32_t events_missed;  /* gaps in the unsolicited sequence                               */
    uint32_t tx_overflow;    /* a frame that would not fit -- never silently truncated          */
    uint32_t busy_rejects;   /* a request refused because one was already in flight             */
} BenchLinkStats;

class BenchLink {
public:
    BenchLink();

    /* `name` is a short label used in every log line about this link, so a message identifies
     * its own subject. `peer` is the BENCH_NODE_* on the far end. */
    /* rxBuf/rxBufLen extend the driver's receive ring on Teensy 4 (ignored elsewhere). Pass a
     * static buffer sized for the link's traffic: at 921600 a byte is 10.9 us, so 8 KB is about
     * 89 ms of headroom against a busy loop(). Pass null to keep the core's default. */
    void begin(HardwareSerial *port, const char *name, uint8_t peer, uint32_t baud,
               uint8_t *rxBuf = 0, size_t rxBufLen = 0);

    void setHandlers(void *ctx, BenchLinkFrameFn onFrame, BenchLinkTimeoutFn onTimeout);

    /* Call every pass through loop(). Drains the UART, runs the parser, and evaluates the
     * request timeout. Non-blocking. */
    void poll();

    /* --- sending --------------------------------------------------------------------------- */

    /* A request expecting a reply. Returns BENCH_ST_BUSY if one is already outstanding, which is
     * a real answer and not an error: the caller must decide whether to wait or give up, because
     * that is a policy question and policy does not belong in a transport. */
    uint8_t request(uint8_t chan, uint8_t cmd, const uint8_t *payload, uint16_t len,
                    uint32_t timeout_ms);

    /* A reply to a request we received. Echoes the request's SEQ, which is what lets the far end
     * match it. */
    uint8_t respond(uint8_t seq, uint8_t chan, uint8_t cmd,
                    const uint8_t *payload, uint16_t len);

    /* An unsolicited announcement. Uses the event sequence class, so the receiver can detect
     * that one went missing. */
    uint8_t event(uint8_t chan, uint8_t cmd, const uint8_t *payload, uint16_t len);

    /* A negative acknowledgement. Rate-limited: a peer that has gone mad and is emitting
     * garbage must not be answered with a NACK per garbage frame, because that doubles the
     * traffic on an already-failing link and can wedge both ends. Suppressed NACKs are COUNTED
     * and reported, never silently dropped -- the count is how you find out it happened. */
    uint8_t nack(uint8_t seq, uint8_t chan, uint8_t cmd, uint8_t error);

    /* --- state ---------------------------------------------------------------------------- */
    bool     pending() const   { return mPending; }
    uint8_t  pendingCmd() const{ return mPendingCmd; }
    bool     peerSeen() const  { return mPeerSeen; }
    uint8_t  peer() const      { return mPeer; }
    const char *name() const   { return mName; }

    /* True once a frame has arrived within the liveness window. This is what "the link is up"
     * means -- not that the cable is plugged in, which nothing can detect, but that the far end
     * spoke recently enough to still be believed. */
    bool isUp(uint32_t now_ms, uint32_t window_ms = 5000u) const {
        return mPeerSeen && (now_ms - mLastRxMs) < window_ms;
    }

    const BenchLinkStats &stats() const { return mStats; }
    const IopParser &parser() const { return mParser; }
    void resetStats();

    /* Re-anchor the event counter. Call on a HELLO from the far end: it restarted, so its
     * counter legitimately went back to the start of the class. */
    void anchorEvents(uint8_t seq) { mLastEventSeq = seq; mEventSeqValid = true; }

private:
    uint8_t sendFrame(uint8_t seq, uint8_t flags, uint8_t chan, uint8_t cmd,
                      const uint8_t *payload, uint16_t len);
    void handleFrame(const IopFrame *f);

    HardwareSerial *mPort;
    const char     *mName;
    uint8_t         mPeer;

    IopParser       mParser;
    IopNackLimiter  mNackLimiter;

    uint8_t         mReqSeq;
    uint8_t         mEvtSeq;

    bool            mPending;
    uint8_t         mPendingSeq;
    uint8_t         mPendingChan;
    uint8_t         mPendingCmd;
    uint32_t        mPendingStartMs;
    uint32_t        mPendingTimeoutMs;

    bool            mPeerSeen;
    uint32_t        mLastRxMs;
    uint8_t         mLastEventSeq;
    bool            mEventSeqValid;

    void               *mCtx;
    BenchLinkFrameFn    mOnFrame;
    BenchLinkTimeoutFn  mOnTimeout;

    BenchLinkStats  mStats;

    /* One frame's worth of assembly buffer, sized from the protocol rather than guessed. Static
     * per link: no allocation anywhere in this system, so there is no failure mode that depends
     * on how long it has been running. */
    uint8_t         mTxBuf[IOP_MAX_FRAME_SIZE];
};

#endif /* BENCH_LINK_H */
