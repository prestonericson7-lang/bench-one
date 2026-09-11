/* ===========================================================================================
 *  teensy2_worker.ino -- BENCH ONE node 3: the worker
 * ===========================================================================================
 *
 *  Teensy 4.1 #2. One link, one job at a time, nothing else.
 *
 *  Serial1 (pins 0/1) <-> Teensy1 Serial3 (pins 14/15), 921600 8N1, framed, CRC-16/MCRF4XX.
 *  Every other pin on this board is free, which is the entire reason it exists: a second
 *  600 MHz Cortex-M7 whose timing nothing else in the system can disturb.
 *
 *
 *  THE ONE DESIGN RULE HERE: JOBS RUN IN SLICES, NEVER TO COMPLETION
 *  ------------------------------------------------------------------
 *  A job request is ACCEPTED and then runs a little at a time, a slice per pass through loop().
 *  The reply to WRK_JOB_REQ says "accepted", not "finished"; completion arrives later as an
 *  unsolicited WRK_JOB_EVENT.
 *
 *  This is not an optimisation, it is the difference between a working link and a wedged one.
 *  The protocol allows exactly ONE outstanding request per link. If this node ran a two-second
 *  job inside the request handler, the master could not talk to it for two seconds -- could not
 *  cancel, could not ask for status, could not find out whether it had crashed. Worse, the
 *  master's own loop would be waiting on a response that had not been sent, so the radio and the
 *  fabric would stall behind a computation neither of them cares about.
 *
 *  Head-of-line blocking is the failure mode. Slicing is the fix, and it costs one state machine.
 *
 *
 *  TIMING DISCIPLINE, INHERITED FROM THIS PROJECT
 *  ----------------------------------------------
 *  Every duration reported here is a delta between two reads of THIS BOARD'S cycle counter.
 *  No figure is ever produced by comparing a timestamp from one board against a timestamp from
 *  another. Two free-running crystals differ by hundreds of ppm and drift with temperature, so
 *  a cross-clock "measurement" reports the drift and calls it latency.
 *
 *  DWT_CYCCNT at 600 MHz gives 1.667 ns per tick. That is the resolution; the accuracy is set by
 *  the crystal, and the crystal is the same one for both ends of every delta.
 * ===========================================================================================
 */

#include <Arduino.h>

#include "interop_protocol.h"
#include "bench_protocol.h"
#include "bench_pins.h"
#include "bench_link.h"

/* ===========================================================================================
 * IDENTITY AND STATE
 * =========================================================================================== */

static BenchLink gMaster;
static uint8_t   gMasterRx[8192];   /* ~89 ms of headroom at 921600                            */

/* Job types. Deliberately dependency-free integer and float work: no library, no allocation,
 * nothing whose version could change what is being measured. Every one of these can be run
 * byte-for-byte identically on the Luckfox and on the ESP32, which is the point -- the
 * comparison is only meaningful if all three are doing the same arithmetic. */
#define JOB_NOP          0x00u   /* accept and finish. Measures the floor: link + dispatch.    */
#define JOB_ECHO         0x01u   /* return the payload. Measures link round-trip with payload. */
#define JOB_CRC16        0x02u   /* CRC-16/MCRF4XX over N bytes. Byte-serial, branchy.         */
#define JOB_SIEVE        0x03u   /* primes below N. Integer, memory-bound.                     */
#define JOB_FLOAT_MADD   0x04u   /* N fused multiply-adds. FPU throughput.                     */
#define JOB_MEMBW        0x05u   /* fill and sum a buffer. Memory bandwidth.                   */
#define JOB_SORT         0x06u   /* sort N pseudo-random uint32. Branch-heavy, cache-hostile.  */

#define JOB_BUF_BYTES    32768u  /* the scratch every job shares                               */

static uint8_t  gJobBuf[JOB_BUF_BYTES];

static struct {
    bool     active;
    uint8_t  id;
    uint8_t  type;
    uint8_t  state;
    uint32_t total;          /* total units of work                                            */
    uint32_t done;           /* units completed so far                                         */
    uint32_t startCycles;
    uint32_t cycles;         /* accumulated cycles spent INSIDE the work, not wall clock       */
    uint32_t result;         /* the job's answer, whatever that means for its type             */
    uint16_t resultLen;      /* bytes of gJobBuf that are the result (ECHO)                    */
} gJob;

static struct {
    uint32_t accepted;
    uint32_t completed;
    uint32_t failed;
    uint32_t cancelled;
    uint32_t rejected;       /* asked while busy                                               */
} gJobStats;

/* Cycles spent in loop() over the last second, used for a crude but honest busy percentage. */
static uint32_t gBusyCycles;
static uint32_t gWindowStartMs;
static uint8_t  gBusyPct;

/* ===========================================================================================
 * CYCLE COUNTER
 * =========================================================================================== */

static inline void cycleCounterBegin(void)
{
    /* The Teensy 4 core enables TRCENA and CYCCNT in startup.c, so this is belt and braces --
     * but it is cheap, and a cycle counter that silently reads zero would make every measurement
     * in this file report an impossible speed rather than an error. */
    ARM_DEMCR |= ARM_DEMCR_TRCENA;
    ARM_DWT_CTRL |= ARM_DWT_CTRL_CYCCNTENA;
}

static inline uint32_t cycles(void) { return ARM_DWT_CYCCNT; }

/* 32 bits of cycles at 600 MHz wraps every 7.158 seconds. Unsigned subtraction handles a single
 * wrap correctly; two wraps do not, so no single measured interval may exceed 7.15 s. Job slices
 * are microseconds, so this is never close -- but it is why the job's total time is accumulated
 * per slice instead of taken as one delta across the whole job. */
static inline uint32_t cyclesToNs(uint32_t c)
{
    /* 64-bit intermediate: c can reach 4.29e9 and multiplying by 1e9 overflows 32 bits
     * immediately. This exact bug is recorded in the master controller's source. */
    return (uint32_t)(((uint64_t)c * 1000000000ull) / (uint64_t)F_CPU_ACTUAL);
}

/* ===========================================================================================
 * PAYLOAD HELPERS -- explicit big-endian, never a struct cast
 * ===========================================================================================
 * Casting a packed struct over a payload works until a compiler pads it differently, and then it
 * fails as a plausible wrong number rather than an error. This project already lost time to
 * hand-counted struct offsets; explicit byte packing costs three lines and cannot drift.
 */
static inline void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static inline uint16_t get16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

/* ===========================================================================================
 * THE JOBS
 * ===========================================================================================
 * Each runs `units` units of work and returns how many it actually did. Slicing keeps the link
 * responsive; the slice size is chosen so one slice is tens of microseconds, far below the
 * 500 ms request timeout and small enough that a cancel lands promptly.
 */

static uint32_t jobSliceCrc16(uint32_t units)
{
    static uint16_t crc;
    if (gJob.done == 0u) { crc = 0xFFFFu; }
    uint32_t n = units;
    for (uint32_t i = 0; i < n; i++) {
        crc = iop_crc16_update(crc, gJobBuf[(gJob.done + i) % JOB_BUF_BYTES]);
    }
    gJob.result = crc;
    return n;
}

static uint32_t jobSliceSieve(uint32_t units)
{
    /* Marks composites in gJobBuf as a bit array. `done` is the candidate cursor, so the job
     * resumes exactly where it stopped -- which is what makes slicing correct rather than just
     * convenient. */
    uint32_t limit = gJob.total;
    uint32_t i = (gJob.done < 2u) ? 2u : gJob.done;
    uint32_t end = i + units;
    if (end > limit) { end = limit; }

    for (; i < end; i++) {
        if (!(gJobBuf[i >> 3] & (1u << (i & 7u)))) {
            for (uint32_t j = i * i; j < limit && j >= i; j += i) {
                gJobBuf[j >> 3] |= (uint8_t)(1u << (j & 7u));
            }
        }
    }

    if (end >= limit) {
        uint32_t count = 0;
        for (uint32_t k = 2; k < limit; k++) {
            if (!(gJobBuf[k >> 3] & (1u << (k & 7u)))) { count++; }
        }
        gJob.result = count;
    }
    return end - ((gJob.done < 2u) ? 2u : gJob.done);
}

static uint32_t jobSliceFloatMadd(uint32_t units)
{
    /* `volatile` on the accumulator, deliberately. Without it the optimiser is entitled to
     * delete the entire loop -- the result is unused until the end -- and the benchmark then
     * reports an astonishing figure that measures nothing at all. A benchmark that got optimised
     * away is the classic articulate wrong answer: it produces a number, in the right units, at
     * the right time. */
    static volatile float acc;
    if (gJob.done == 0u) { acc = 1.0f; }
    float a = (float)acc;
    for (uint32_t i = 0; i < units; i++) {
        a = a * 1.0000001f + 0.0000001f;
    }
    acc = a;
    gJob.result = (uint32_t)(a * 1000.0f);
    return units;
}

static uint32_t jobSliceMembw(uint32_t units)
{
    static uint32_t sum;
    if (gJob.done == 0u) { sum = 0; }
    uint32_t *w = (uint32_t *)gJobBuf;
    const uint32_t words = JOB_BUF_BYTES / 4u;
    for (uint32_t i = 0; i < units; i++) {
        uint32_t idx = (gJob.done + i) % words;
        w[idx] = w[idx] * 1664525u + 1013904223u;
        sum += w[idx];
    }
    gJob.result = sum;
    return units;
}

static uint32_t jobSliceSort(uint32_t units)
{
    /* An insertion-sort pass per slice. Quadratic on purpose: it is branch-heavy and
     * cache-hostile, which is exactly the workload profile that separates a Cortex-M7 from a
     * Cortex-A7 differently than raw arithmetic does. Comparing the same quadratic sort across
     * all three processors is more informative than comparing three different clever sorts. */
    uint32_t *w = (uint32_t *)gJobBuf;
    uint32_t n = gJob.total;
    if (n > JOB_BUF_BYTES / 4u) { n = JOB_BUF_BYTES / 4u; }

    uint32_t i = gJob.done + 1u;
    uint32_t end = i + units;
    if (end > n) { end = n; }

    for (; i < end; i++) {
        uint32_t key = w[i];
        int32_t j = (int32_t)i - 1;
        while (j >= 0 && w[j] > key) { w[j + 1] = w[j]; j--; }
        w[j + 1] = key;
    }
    gJob.result = w[0];
    return end - (gJob.done + 1u);
}

/* ===========================================================================================
 * JOB CONTROL
 * =========================================================================================== */

static void jobFinish(uint8_t state)
{
    uint8_t p[16];
    p[0] = gJob.id;
    p[1] = state;
    p[2] = 100u;
    put32(&p[3], gJob.result);
    put32(&p[7], cyclesToNs(gJob.cycles));
    put32(&p[11], gJob.done);
    p[15] = gJob.type;

    gMaster.event(BENCH_CHAN_WORKER, BENCH_CMD_WRK_JOB_EVENT, p, 16);

    if (state == BENCH_JOB_STATE_DONE)      { gJobStats.completed++; }
    else if (state == BENCH_JOB_STATE_FAILED) { gJobStats.failed++; }

    gJob.active = false;
}

static void jobStep(void)
{
    if (!gJob.active) { return; }

    /* Slice size is a unit count, not a time budget, because a time budget needs a clock read
     * per iteration and that read costs more than the work for the cheap jobs. These counts were
     * chosen so one slice is roughly 10-50 us at 600 MHz. */
    uint32_t units;
    switch (gJob.type) {
        case JOB_CRC16:      units = 4096u; break;
        case JOB_SIEVE:      units = 512u;  break;
        case JOB_FLOAT_MADD: units = 8192u; break;
        case JOB_MEMBW:      units = 4096u; break;
        case JOB_SORT:       units = 64u;   break;
        default:             units = 1u;    break;
    }

    uint32_t remaining = (gJob.total > gJob.done) ? (gJob.total - gJob.done) : 0u;
    if (units > remaining) { units = remaining; }

    uint32_t t0 = cycles();
    uint32_t did = 0;
    switch (gJob.type) {
        case JOB_CRC16:      did = jobSliceCrc16(units);     break;
        case JOB_SIEVE:      did = jobSliceSieve(units);     break;
        case JOB_FLOAT_MADD: did = jobSliceFloatMadd(units); break;
        case JOB_MEMBW:      did = jobSliceMembw(units);     break;
        case JOB_SORT:       did = jobSliceSort(units);      break;
        default:             did = remaining;                break;
    }
    /* Accumulated per slice, so the total never spans a 7.15 s counter wrap even for a job that
     * takes minutes of wall-clock time. It also excludes the link handling between slices, which
     * is the honest thing to report: this is time spent computing, not time spent existing. */
    gJob.cycles += (cycles() - t0);

    gJob.done += did;

    if (gJob.done >= gJob.total || did == 0u) {
        jobFinish(BENCH_JOB_STATE_DONE);
    }
}

/* ===========================================================================================
 * FRAME HANDLING
 * =========================================================================================== */

static void sendHello(void)
{
    uint8_t p[10];
    p[0] = BENCH_NODE_TEENSY2;
    p[1] = IOP_PROTOCOL_VERSION;
    p[2] = IOP_PROTOCOL_MINOR;
    p[3] = BENCH_PROTOCOL_VERSION;
    p[4] = 0;                                  /* reset reason: see note below                 */
    put16(&p[5], 0x0000u);                     /* capability bits, none defined for the worker */
    put16(&p[7], IOP_CRC16_CHECK_VALUE);       /* the far end compares this against its own    */
    p[9] = BENCH_FABRIC_HW_REV;

    gMaster.event(BENCH_CHAN_WORKER, BENCH_CMD_WRK_HELLO, p, 10);
}

/* The CRC check value travels in the HELLO on purpose. A header that has drifted between two
 * sketch folders produces a stream of CRC failures that looks exactly like a wiring fault, and
 * this project has already paid for that lesson once. Comparing check values on the first frame
 * of a session turns it into one line of plain English. */

static void handleJobRequest(const IopFrame *f)
{
    uint8_t resp[2];

    if (f->payload_len < 8u) {
        resp[0] = BENCH_ST_BAD_ARG; resp[1] = 0;
        gMaster.respond(f->seq, BENCH_CHAN_WORKER, BENCH_CMD_WRK_JOB_RESP, resp, 2);
        return;
    }

    uint8_t  id    = f->payload[0];
    uint8_t  type  = f->payload[1];
    uint32_t total = get32(&f->payload[2]);
    uint16_t alen  = get16(&f->payload[6]);

    if (gJob.active) {
        /* Refused, not queued. One job at a time is the whole contract, and a queue here would
         * hide from the master that it is asking faster than this node can answer. */
        gJobStats.rejected++;
        resp[0] = BENCH_ST_BUSY; resp[1] = gJob.id;
        gMaster.respond(f->seq, BENCH_CHAN_WORKER, BENCH_CMD_WRK_JOB_RESP, resp, 2);
        return;
    }

    memset(&gJob, 0, sizeof(gJob));
    gJob.id   = id;
    gJob.type = type;

    switch (type) {
        case JOB_NOP:
            gJob.total = 1u;
            break;

        case JOB_ECHO: {
            uint16_t n = alen;
            if (n > (uint16_t)(f->payload_len - 8u)) { n = (uint16_t)(f->payload_len - 8u); }
            if (n > JOB_BUF_BYTES) { n = JOB_BUF_BYTES; }
            memcpy(gJobBuf, &f->payload[8], n);
            gJob.resultLen = n;
            gJob.total = 1u;
            break;
        }

        case JOB_SIEVE:
            /* The sieve's bit array is one bit per candidate, so the limit is bounded by the
             * buffer. Clamped rather than rejected: a caller asking for more gets the largest
             * honest answer plus a `total` in the completion event that says what was actually
             * run. Silently doing less than asked without saying so is the thing to avoid. */
            if (total > JOB_BUF_BYTES * 8u) { total = JOB_BUF_BYTES * 8u; }
            memset(gJobBuf, 0, JOB_BUF_BYTES);
            gJob.total = total;
            break;

        case JOB_SORT: {
            if (total > JOB_BUF_BYTES / 4u) { total = JOB_BUF_BYTES / 4u; }
            uint32_t *w = (uint32_t *)gJobBuf;
            uint32_t s = 0x12345678u;
            for (uint32_t i = 0; i < total; i++) {
                s = s * 1664525u + 1013904223u;   /* fixed seed: the same data every run, so a
                                                   * repeat measurement is comparable          */
                w[i] = s;
            }
            gJob.total = total;
            break;
        }

        case JOB_CRC16:
        case JOB_FLOAT_MADD:
        case JOB_MEMBW:
            gJob.total = total;
            break;

        default:
            resp[0] = BENCH_ST_UNSUPPORTED; resp[1] = id;
            gMaster.respond(f->seq, BENCH_CHAN_WORKER, BENCH_CMD_WRK_JOB_RESP, resp, 2);
            return;
    }

    gJob.active      = true;
    gJob.startCycles = cycles();
    gJobStats.accepted++;

    /* ACCEPTED, not FINISHED. The distinction is the whole reason this node stays responsive. */
    resp[0] = BENCH_ST_OK; resp[1] = id;
    gMaster.respond(f->seq, BENCH_CHAN_WORKER, BENCH_CMD_WRK_JOB_RESP, resp, 2);
}

static void handleResultRequest(const IopFrame *f)
{
    if (f->payload_len < 7u) { return; }

    uint8_t  id     = f->payload[0];
    uint32_t offset = get32(&f->payload[1]);
    uint16_t maxLen = get16(&f->payload[5]);

    uint8_t hdr[8];
    if (id != gJob.id) {
        hdr[0] = BENCH_ST_NO_SUCH_JOB; hdr[1] = id;
        put32(&hdr[2], offset); put16(&hdr[6], 0);
        gMaster.respond(f->seq, BENCH_CHAN_WORKER, BENCH_CMD_WRK_RESULT_RESP, hdr, 8);
        return;
    }

    uint16_t avail = gJob.resultLen;
    if (offset >= avail) { maxLen = 0; }
    else {
        uint16_t left = (uint16_t)(avail - offset);
        if (maxLen > left) { maxLen = left; }
    }
    /* Bounded by the frame, not by hope. The builder would refuse an oversize payload anyway,
     * but clamping here means the caller gets a short read it can page through rather than an
     * OVERFLOW it has to interpret. */
    if (maxLen > (uint16_t)(IOP_MAX_PAYLOAD_LEN - 8u)) {
        maxLen = (uint16_t)(IOP_MAX_PAYLOAD_LEN - 8u);
    }

    static uint8_t out[IOP_MAX_PAYLOAD_LEN];
    out[0] = BENCH_ST_OK; out[1] = id;
    put32(&out[2], offset); put16(&out[6], maxLen);
    if (maxLen) { memcpy(&out[8], &gJobBuf[offset], maxLen); }

    gMaster.respond(f->seq, BENCH_CHAN_WORKER, BENCH_CMD_WRK_RESULT_RESP,
                    out, (uint16_t)(8u + maxLen));
}

static void handleStatusRequest(const IopFrame *f)
{
    uint8_t p[20];
    p[0] = BENCH_ST_OK;
    p[1] = gJob.active ? 1u : 0u;
    p[2] = 0;                                    /* queue depth: always 0, there is no queue   */
    put32(&p[3], gJobStats.completed);
    put32(&p[7], gJobStats.failed);
    p[11] = gBusyPct;
    put32(&p[12], (uint32_t)0);                  /* free RAM: see note below                   */
    put32(&p[16], millis());
    gMaster.respond(f->seq, BENCH_CHAN_WORKER, BENCH_CMD_WRK_STATUS_RESP, p, 20);
}

/* Free RAM is reported as 0 rather than estimated. Nothing here allocates -- every buffer is
 * static -- so "free RAM" would be a constant computed from linker symbols and dressed up as a
 * measurement. Reporting a real zero is more honest than reporting a fake number, and if
 * allocation is ever introduced this is the field that gets filled in. */

static void handleBenchRequest(const IopFrame *f)
{
    if (f->payload_len < 5u) { return; }

    uint8_t  kind  = f->payload[0];
    uint32_t iters = get32(&f->payload[1]);
    if (iters == 0u) { iters = 1u; }
    if (iters > 10000u) { iters = 10000u; }

    /* Runs to completion inside the handler, unlike a job, and that is a deliberate exception:
     * a benchmark measures uninterrupted execution, so slicing it would measure the slicing.
     * The iteration count is clamped so the worst case stays well inside the master's
     * BENCH_TIMEOUT_WORKER_BENCH_MS of 30 s. */
    uint32_t mn = 0xFFFFFFFFu, mx = 0; uint64_t sum = 0;

    for (uint32_t i = 0; i < iters; i++) {
        uint32_t t0 = cycles();
        switch (kind) {
            case JOB_CRC16: {
                uint16_t c = 0xFFFFu;
                for (uint16_t k = 0; k < 256u; k++) { c = iop_crc16_update(c, (uint8_t)k); }
                gJob.result = c;
                break;
            }
            case JOB_FLOAT_MADD: {
                volatile float a = 1.0f;
                for (uint16_t k = 0; k < 256u; k++) { a = a * 1.0000001f + 0.0000001f; }
                break;
            }
            default: {
                volatile uint32_t x = 0;
                for (uint16_t k = 0; k < 256u; k++) { x += k; }
                break;
            }
        }
        uint32_t d = cycles() - t0;
        if (d < mn) { mn = d; }
        if (d > mx) { mx = d; }
        sum += d;
    }

    uint32_t meanCycles = (uint32_t)(sum / iters);
    uint32_t meanNs = cyclesToNs(meanCycles);

    /* 22 bytes: status(1) + kind(1) + min(4) + mean(4) + max(4) + iters(4) + ops_per_s(4).
     *
     * The first draft declared p[21] and then wrote a 32-bit field at offset 18, which needs
     * bytes 18 through 21 -- one past the end. The compiler caught it with -Warray-bounds and it
     * is worth recording why it mattered: a one-byte stack overrun does not crash, it corrupts
     * whatever the compiler happened to place next, and the symptom would have surfaced
     * somewhere unrelated hours later. Counting the payload out in a comment, as above, is what
     * stops that recurring. */
    uint8_t p[22];
    p[0] = BENCH_ST_OK;
    p[1] = kind;
    put32(&p[2],  cyclesToNs(mn));
    put32(&p[6],  meanNs);
    put32(&p[10], cyclesToNs(mx));
    put32(&p[14], iters);

    /* Operations per second, derived from the MEAN rather than the minimum. Quoting the minimum
     * is how benchmarks flatter themselves: it reports the one iteration that dodged every
     * interrupt, which is not a rate anything can sustain.
     *
     * Guard the division. meanNs can be 0 if the optimiser removed the work, and that would be a
     * divide by zero reporting infinite throughput -- exactly the articulate wrong answer the
     * volatile accumulators above exist to prevent. Reporting 0 ops/s instead is obviously
     * broken, which is the point. */
    put32(&p[18], (meanNs > 0u)
                  ? (uint32_t)(1000000000ull * 256ull / (uint64_t)meanNs)
                  : 0u);

    gMaster.respond(f->seq, BENCH_CHAN_WORKER, BENCH_CMD_WRK_BENCH_RESP, p, 22);
}

static void onFrame(void *ctx, const IopFrame *f, bool solicited)
{
    (void)ctx; (void)solicited;

    /* Transport is answered on every channel, because PING is how the far end proves this node
     * is alive without changing anything about its state. */
    if (f->chan == IOP_CHAN_TRANSPORT) {
        if (f->cmd == IOP_CMD_PING) {
            gMaster.respond(f->seq, IOP_CHAN_TRANSPORT, IOP_CMD_PONG, 0, 0);
        }
        return;
    }

    if (f->chan != BENCH_CHAN_WORKER) {
        /* A channel this node does not implement. Answered on the wire, not dropped: a silent
         * drop leaves the master waiting for its full timeout, and the protocol is symmetric
         * precisely so a peer can say "I do not do that" immediately. */
        gMaster.nack(f->seq, f->chan, f->cmd, IOP_ERR_UNKNOWN_CHAN);
        return;
    }

    switch (f->cmd) {
        case BENCH_CMD_WRK_JOB_REQ:    handleJobRequest(f);    break;
        case BENCH_CMD_WRK_RESULT_REQ: handleResultRequest(f); break;
        case BENCH_CMD_WRK_STATUS_REQ: handleStatusRequest(f); break;
        case BENCH_CMD_WRK_BENCH_REQ:  handleBenchRequest(f);  break;

        case BENCH_CMD_WRK_CANCEL_REQ: {
            uint8_t resp[2];
            uint8_t id = (f->payload_len > 0u) ? f->payload[0] : 0u;
            if (gJob.active && gJob.id == id) {
                gJobStats.cancelled++;
                jobFinish(BENCH_JOB_STATE_CANCELLED);
                resp[0] = BENCH_ST_OK;
            } else {
                resp[0] = BENCH_ST_NO_SUCH_JOB;
            }
            resp[1] = id;
            gMaster.respond(f->seq, BENCH_CHAN_WORKER, BENCH_CMD_WRK_CANCEL_RESP, resp, 2);
            break;
        }

        default:
            gMaster.nack(f->seq, f->chan, f->cmd, IOP_ERR_UNKNOWN_CMD);
            break;
    }
}

static void onTimeout(void *ctx, uint8_t chan, uint8_t cmd, uint8_t seq)
{
    (void)ctx; (void)chan; (void)cmd; (void)seq;
    /* This node initiates no requests, so a timeout here would mean the link class has a bug.
     * The hook exists so that if that ever changes, there is a place for it rather than a
     * silently ignored condition. */
}

/* ===========================================================================================
 * SETUP AND LOOP
 * =========================================================================================== */

void setup(void)
{
    cycleCounterBegin();

    pinMode(T2_LA_MARK, OUTPUT);
    digitalWriteFast(T2_LA_MARK, LOW);

    pinMode(LED_BUILTIN, OUTPUT);

    /* The protocol's own power-on self-test. Ten milliseconds of arithmetic that turns "the link
     * does not work" -- the least informative symptom imaginable -- into a specific answer about
     * whether the fault is in this file or in the wiring. If it fails, the wiring is innocent. */
    static IopParser scratch;
    uint16_t failures = iop_selftest(&scratch);

    gMaster.begin(&T2_MASTER_SERIAL, "master", BENCH_NODE_TEENSY1, BENCH_LINK_BAUD,
                  gMasterRx, sizeof(gMasterRx));
    gMaster.setHandlers(0, onFrame, onTimeout);

    memset(&gJob, 0, sizeof(gJob));
    memset(&gJobStats, 0, sizeof(gJobStats));
    gWindowStartMs = millis();

    if (failures != 0u) {
        /* Announce the failure on the wire and then blink it forever. There is no point running:
         * every frame this node builds would be malformed, and it would present as a wiring
         * fault on the other board. Better to be obviously dead than subtly wrong. */
        for (;;) {
            uint8_t p[3] = { BENCH_NODE_TEENSY2,
                             (uint8_t)(failures >> 8), (uint8_t)failures };
            gMaster.event(BENCH_CHAN_WORKER, BENCH_CMD_WRK_HELLO, p, 3);
            digitalWriteFast(LED_BUILTIN, HIGH); delay(100);
            digitalWriteFast(LED_BUILTIN, LOW);  delay(100);
        }
    }

    sendHello();
}

void loop(void)
{
    uint32_t t0 = cycles();

    gMaster.poll();
    jobStep();

    gBusyCycles += (cycles() - t0);

    /* A one-second window turned into a percentage. Crude, and honest about being crude: it
     * counts cycles spent inside loop() against wall-clock cycles, so it includes the link
     * polling and excludes anything in an interrupt. It is a load indicator, not a profile. */
    uint32_t now = millis();
    if ((now - gWindowStartMs) >= 1000u) {
        uint64_t elapsedCycles = (uint64_t)(now - gWindowStartMs) * (F_CPU_ACTUAL / 1000ull);
        gBusyPct = (elapsedCycles > 0u)
                   ? (uint8_t)((100ull * (uint64_t)gBusyCycles) / elapsedCycles) : 0u;
        if (gBusyPct > 100u) { gBusyPct = 100u; }
        gBusyCycles = 0;
        gWindowStartMs = now;

        /* Heartbeat: the LED says this node is looping. A node that has hung with the link up
         * looks identical to an idle one over the wire, and this is the cheapest way to tell
         * them apart from across the desk. */
        digitalWriteFast(LED_BUILTIN, gJob.active ? HIGH
                                                  : !digitalReadFast(LED_BUILTIN));
    }
}
