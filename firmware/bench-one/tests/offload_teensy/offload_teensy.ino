/* ===========================================================================================
 *  offload_teensy -- does taking work OFF a Teensy make its memory faster, and by how much
 * ===========================================================================================
 *
 *  THE QUESTION THIS ANSWERS
 *  -------------------------
 *  The architecture proposal is: let a Luckfox do all the talking so a Teensy does nothing but matrix
 *  arithmetic, and the Teensy's memory then goes faster because that is all it is doing.
 *
 *  Half of that is wrong and half is right, and the difference is worth measuring rather than arguing.
 *
 *  WRONG: the memory does not get faster. 66.5 MB/s of FlexSPI2 and 39.3 MB/s of nibble unpacking are
 *  properties of the bus and the core. They do not depend on how busy the core is with anything else.
 *
 *  RIGHT, and bigger than the thing asked for: read and compute currently ADD rather than overlap,
 *  because the same core issues the loads and does the arithmetic. That is measured -- it is why PSRAM
 *  reads at 33.9 MB/s and delivers 18.2 MB/s of useful work. If something OTHER than the core moves
 *  the bytes, the two stop adding, and that is worth up to 2x.
 *
 *  The something-else is eDMA, on the same chip, not a second board. Whether that actually works is
 *  case 4 below, and it is a real question rather than a formality -- see the DTCM warning.
 *
 *
 *  FIVE CASES, AND WHAT EACH ONE ISOLATES
 *  --------------------------------------
 *   1  MAC from DTCM                     the pure compute rate. No memory system involved.
 *   2  MAC from OCRAM                    the same arithmetic one bus hop further away.
 *   3  read PSRAM + unpack + MAC          what the machine does TODAY. Expect roughly 18 MB/s.
 *   4  eDMA PSRAM -> OCRAM, MAC from OCRAM  the overlap claim. Does it beat case 3?
 *   5  case 3 plus servicing a USB link    the cost of "doing the talking", which is the number that
 *                                          decides whether offloading it to a Luckfox is worth a hop.
 *
 *  Case 5 minus case 3 is the answer to the original question. If the link costs 20% of wall clock,
 *  removing it multiplies throughput by 1.25 and nothing else changes. If it costs 2%, the Luckfox
 *  earns its place some other way or not at all.
 *
 *
 *  A CONSTRAINT THAT BREAKS THE OBVIOUS DESIGN
 *  -------------------------------------------
 *  **eDMA cannot touch DTCM.** On the i.MX RT1062 the tightly-coupled memories hang off the core's own
 *  ports, not off the AXI fabric that eDMA masters. So the fastest memory on the chip is exactly the
 *  one a DMA engine cannot fill, and any "DMA into DTCM while the core computes" scheme is not a
 *  tuning problem, it is impossible.
 *
 *  Case 4 therefore double-buffers in OCRAM, which eDMA can reach. Case 2 exists to price that
 *  decision: if OCRAM arithmetic is much slower than DTCM arithmetic, overlapping may cost more than
 *  it saves, and case 2 minus case 1 is exactly that cost.
 *
 *  Whether eDMA can read the FlexSPI window at all is also not assumed. The sketch checks the
 *  transferred bytes against the source and says so. A silent zero-fill would otherwise look like a
 *  spectacular throughput result.
 *
 *
 *  THE KERNEL IS THE REAL ONE
 *  --------------------------
 *  4-bit weights, two per byte, unpacked to signed and accumulated against an activation vector. That
 *  is the inner loop of the whole machine, and it is the same kernel measured at 39.3 MB/s in doc 23.
 *  A synthetic loop would make every number here meaningless.
 *
 *  RUN
 *      Flash, open the serial monitor at any baud, and read the table. Needs PSRAM fitted for cases
 *      3 to 5; it says so and skips them if not.
 * ========================================================================================= */

#include <Arduino.h>
#include <DMAChannel.h>

/* How much PSRAM the core found at boot, in MB. Declared by the Teensy core's startup, not by a
 * header, which is why it needs spelling out here. */
extern "C" uint8_t external_psram_size;

/* ---- sizes -------------------------------------------------------------------------------
 * CHUNK is one DMA transfer and one MAC block. 4 KB is chosen because it is large enough that
 * per-transfer overhead is not the story and small enough that two of them fit in OCRAM beside
 * everything else. BYTES is the total streamed per case: big enough that the measurement is not
 * dominated by cache effects, small enough to finish in a couple of seconds.
 * ---------------------------------------------------------------------------------------- */
#define CHUNK      4096
#define BYTES      (4u << 20)          /* 4 MB per case */
#define VEC        64                  /* activation vector length, matches the real kernel */

/* DTCM. Teensy puts ordinary globals here, which is why this one has no attribute. */
static uint8_t  dtcm_buf[CHUNK];
static int8_t   act[VEC];

/* OCRAM. DMAMEM puts it in RAM2, which is the part eDMA can actually reach. */
DMAMEM static uint8_t ocram_buf[CHUNK];
DMAMEM static uint8_t dma_buf[2][CHUNK];

static DMAChannel dma;

/* ---- the kernel ---------------------------------------------------------------------------
 * Two 4-bit weights per byte, each unpacked to a signed value in [-8, 7], multiplied by an
 * activation and accumulated. Returns the accumulator so the optimiser cannot delete the work,
 * which is the single easiest way to produce a fictitious benchmark.
 * ---------------------------------------------------------------------------------------- */
static inline int32_t mac_block(const uint8_t *w, uint32_t n, const int8_t *a)
{
    int32_t acc = 0;
    uint32_t ai = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t b = w[i];
        int32_t lo = (int32_t)(b & 0x0F) - 8;
        int32_t hi = (int32_t)(b >> 4)   - 8;
        acc += lo * a[ai];
        ai = (ai + 1) & (VEC - 1);
        acc += hi * a[ai];
        ai = (ai + 1) & (VEC - 1);
    }
    return acc;
}

static volatile int32_t sink;          /* volatile so the accumulator cannot be optimised away */

static void fill(uint8_t *p, uint32_t n, uint32_t seed)
{
    for (uint32_t i = 0; i < n; i++) p[i] = (uint8_t)(i * 0x9D + seed);
}

/* ---- timing ------------------------------------------------------------------------------ */
static float rate_mbs(uint32_t bytes, uint32_t us)
{
    return (us == 0) ? 0.0f : (float)bytes / (float)us;   /* bytes/us == MB/s */
}

/* =========================================================================================
 *  case 1 and 2 -- arithmetic out of each memory, no streaming at all
 * ====================================================================================== */
static float mac_from(uint8_t *buf)
{
    uint32_t reps = BYTES / CHUNK;
    uint32_t t0 = micros();
    for (uint32_t r = 0; r < reps; r++) sink += mac_block(buf, CHUNK, act);
    uint32_t t1 = micros();
    return rate_mbs(reps * CHUNK, t1 - t0);
}

/* =========================================================================================
 *  case 3 -- read external memory and compute in the same loop. Today's path.
 *
 *  The copy into a local buffer is NOT an artificial step: the kernel wants sequential access and
 *  the core has to issue those loads either way. Doing it as a block copy is the fastest form of
 *  what the core must do, so this is a generous measurement of the current design rather than a
 *  pessimistic one.
 * ====================================================================================== */
static float read_and_mac(volatile uint8_t *src, uint32_t total)
{
    uint32_t t0 = micros();
    for (uint32_t off = 0; off < total; off += CHUNK) {
        memcpy(dtcm_buf, (const void *)(src + (off % (1u << 20))), CHUNK);
        sink += mac_block(dtcm_buf, CHUNK, act);
    }
    uint32_t t1 = micros();
    return rate_mbs(total, t1 - t0);
}

/* =========================================================================================
 *  case 4 -- eDMA fills one OCRAM buffer while the core computes from the other
 *
 *  This is the whole point of the sketch. If it does not beat case 3, the overlap idea is dead and
 *  no amount of moving work to another board changes that, because the bottleneck was never the
 *  other work.
 * ====================================================================================== */
static float dma_and_mac(volatile uint8_t *src, uint32_t total, bool *ok, uint32_t *waited_us)
{
    *ok = true;
    *waited_us = 0;

    /* Prime buffer 0 synchronously so the first compute has something real to chew on. */
    memcpy(dma_buf[0], (const void *)src, CHUNK);

    uint32_t t0 = micros();
    uint32_t cur = 0;
    for (uint32_t off = 0; off < total; off += CHUNK) {
        uint32_t nxt = cur ^ 1u;
        uint32_t soff = (off + CHUNK) % (1u << 20);

        /* Start the NEXT chunk moving before computing on this one. That ordering is the entire
         * mechanism: issue the transfer, then spend the transfer time doing arithmetic. */
        dma.begin(true);
        dma.sourceBuffer((volatile const uint8_t *)(src + soff), CHUNK);
        dma.destinationBuffer(dma_buf[nxt], CHUNK);
        dma.enable();

        sink += mac_block(dma_buf[cur], CHUNK, act);

        /* Whatever is left of the transfer after the arithmetic finished is the part that did NOT
         * overlap. Timing it separately turns "is it faster" into "how much of it overlapped". */
        uint32_t w0 = micros();
        uint32_t guard = 0;
        while (!dma.complete()) {
            if (++guard > 40000000u) { *ok = false; break; }
        }
        *waited_us += micros() - w0;
        dma.clearComplete();
        if (!*ok) break;
        cur = nxt;
    }
    uint32_t t1 = micros();
    return rate_mbs(total, t1 - t0);
}

/* =========================================================================================
 *  case 5 -- today's path, plus the link traffic that a Luckfox would take away
 *
 *  One 64-byte status frame per 4 KB chunk, written over USB. That is what "doing the talking" looks
 *  like at the rate the pipeline actually talks: a header per block, not a byte per byte. The host's
 *  serial monitor is draining it, so this is a real transfer and not a write into a buffer nobody
 *  empties.
 * ====================================================================================== */
static float read_mac_and_talk(volatile uint8_t *src, uint32_t total)
{
    uint8_t frame[64];
    for (uint32_t i = 0; i < sizeof(frame); i++) frame[i] = 0x20 + (i % 60);
    frame[sizeof(frame) - 1] = '\r';

    uint32_t t0 = micros();
    for (uint32_t off = 0; off < total; off += CHUNK) {
        memcpy(dtcm_buf, (const void *)(src + (off % (1u << 20))), CHUNK);
        sink += mac_block(dtcm_buf, CHUNK, act);
        /* The link work: check for an inbound command, then report. Both halves matter -- polling is
         * cheap and the write is not. */
        if (Serial.available()) (void)Serial.read();
        Serial.write(frame, sizeof(frame));
    }
    Serial.flush();
    uint32_t t1 = micros();
    return rate_mbs(total, t1 - t0);
}

/* ====================================================================================== */

void setup()
{
    Serial.begin(115200);
    while (!Serial && millis() < 4000) { }
    delay(200);

    for (uint32_t i = 0; i < VEC; i++) act[i] = (int8_t)(i - 32);
    fill(dtcm_buf, CHUNK, 0x3B);
    fill(ocram_buf, CHUNK, 0x3B);

    Serial.println();
    Serial.println("=== does offloading work from a Teensy make its memory faster? ===");
    Serial.printf("  CPU %lu MHz, PSRAM %u MB, %lu kB per case, 4-bit unpack + MAC kernel\n",
                  (unsigned long)(F_CPU_ACTUAL / 1000000), external_psram_size,
                  (unsigned long)(BYTES / 1024));
    Serial.println();

    /* ---- cases 1 and 2: arithmetic alone, out of each on-chip memory ---- */
    float c1 = mac_from(dtcm_buf);
    float c2 = mac_from(ocram_buf);

    Serial.println("  case                                          MB/s");
    Serial.printf("  1  MAC from DTCM, no memory system             %6.1f\n", c1);
    Serial.printf("  2  MAC from OCRAM, one bus hop further         %6.1f\n", c2);

    if (external_psram_size == 0) {
        Serial.println();
        Serial.println("  NO PSRAM FITTED. Cases 3 to 5 need external memory and are skipped.");
        Serial.println("  Cases 1 and 2 above are still valid and still answer half the question:");
        Serial.println("  compute alone is the ceiling that offloading can approach and never pass.");
        return;
    }

    volatile uint8_t *psram = (volatile uint8_t *)0x70000000;
    for (uint32_t i = 0; i < (1u << 20); i++) psram[i] = (uint8_t)(i * 0x9D + 0x3B);
    arm_dcache_flush_delete((void *)psram, 1u << 20);

    float c3 = read_and_mac(psram, BYTES);
    Serial.printf("  3  read PSRAM + unpack + MAC  (today)          %6.1f\n", c3);

    bool ok = false; uint32_t waited = 0;
    float c4 = dma_and_mac(psram, BYTES, &ok, &waited);
    if (!ok) {
        Serial.println("  4  eDMA overlap                               FAILED: transfer never completed");
        Serial.println("     eDMA could not read the FlexSPI window. The overlap route is closed on");
        Serial.println("     this path, and case 3 is the real ceiling for a single Teensy.");
    } else {
        /* Verify the DMA actually moved the right bytes. A silent zero-fill would read as a
         * spectacular result, and checking costs nothing. */
        uint32_t bad = 0;
        for (uint32_t i = 0; i < CHUNK; i++)
            if (dma_buf[0][i] != (uint8_t)((i * 0x9D) + 0x3B)) bad++;
        Serial.printf("  4  eDMA PSRAM -> OCRAM, MAC from OCRAM        %6.1f\n", c4);
        Serial.printf("     of which %lu us was spent waiting on DMA that had not overlapped\n",
                      (unsigned long)waited);
        if (bad) Serial.printf("     WARNING: %lu of %d bytes wrong -- treat the rate as meaningless\n",
                               (unsigned long)bad, CHUNK);
        else     Serial.println("     bytes verified against the source, so the rate is real");
    }

    float c5 = read_mac_and_talk(psram, BYTES);
    Serial.printf("  5  case 3 plus a 64-byte frame per 4 kB        %6.1f\n", c5);

    /* ---- what it all means, computed rather than asserted ---- */
    Serial.println();
    Serial.println("  --- what this says about moving work to a Luckfox ---");

    float link_cost = (c3 > 0.0f) ? (1.0f - c5 / c3) : 0.0f;
    Serial.printf("  link handling costs %.1f%% of wall clock, so removing it multiplies\n",
                  link_cost * 100.0f);
    Serial.printf("  throughput by %.2fx and nothing else changes.\n",
                  (link_cost < 0.99f) ? 1.0f / (1.0f - link_cost) : 0.0f);

    if (ok && c4 > c3) {
        Serial.printf("  eDMA overlap is worth %.2fx on its own, ON THE SAME CHIP, with no hop.\n",
                      c4 / c3);
        Serial.println("  That is the bigger lever and it needs no second board. Do it first.");
    } else if (ok) {
        Serial.println("  eDMA overlap did NOT beat the plain loop. Either the transfer does not");
        Serial.println("  overlap on this path or OCRAM arithmetic gives back what the overlap wins:");
        Serial.printf("  case 2 is %.2fx of case 1, which is that cost measured directly.\n",
                      (c1 > 0.0f) ? c2 / c1 : 0.0f);
    }
    Serial.printf("  the ceiling either way is case 1, %.1f MB/s: arithmetic with no memory system\n", c1);
    Serial.println("  at all. No amount of offloading passes it.");
    Serial.println();
    Serial.println("=== done ===");
}

void loop() { }
