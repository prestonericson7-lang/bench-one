/* ===========================================================================================
 *  psram_bank_teensy.ino -- seven PSRAM chips on one Teensy, and proof the bank switch is real
 * ===========================================================================================
 *
 *  psram_teensy.ino tests the one or two chips Teensyduino knows about. This tests a BANK: six chips
 *  sharing one chip-select line through a 74HC138, so the FlexSPI2 controller still believes it is
 *  talking to a single 8 MB device and software chooses which physical chip answers.
 *
 *  One onboard chip on SS0 plus six in the bank on SS1 is 56 MB on one Teensy, at the full 32.8 MB/s
 *  already measured on a single chip. Wiring is in docs/22-psram-bank-wiring.md.
 *
 *
 *  THE TEST THAT ACTUALLY PROVES ANYTHING
 *  ---------------------------------------
 *  Writing a bank and immediately reading it back PASSES WITH THE DECODER DISCONNECTED. The data never
 *  leaves the processor's cache, so the test measures the cache and reports success.
 *
 *  So this writes EVERY bank first, and only then reads any of them. If the decoder is dead, all six
 *  writes went to the same physical chip, the last one wins, and five banks fail. That is the whole
 *  design of the test and it is the reason it is worth running.
 *
 *
 *  THE CACHE IS THE TRAP
 *  ----------------------
 *  The banked window is cached, write-back. Change the three address pins without telling the cache
 *  and the processor serves the PREVIOUS bank's data from cache, with no error and no warning, and any
 *  pending writes land in whichever chip happens to be selected when the line is finally evicted.
 *
 *  Plausible wrong data is the worst failure mode this project has hit before -- a permute that was
 *  not safe in place returned a believable vector and looked like a property of the algorithm. So the
 *  flush is inside psram_bank_select() where it cannot be forgotten, and its COST is measured below,
 *  because flushing 8 MB of cache is not free and it sets how often a bank may be switched.
 *
 *
 *  BOOT ORDER MATTERS: FIT THE PULLDOWNS
 *  --------------------------------------
 *  Teensyduino probes QSPI before setup() runs, so whichever bank the address pins happen to select at
 *  power-on is the one it finds. Floating pins can select an unconnected decoder output, no chip
 *  answers on SS1, and the core reports 8 MB instead of 16 -- which looks exactly like a bad solder
 *  joint. Three 10k resistors from A, B and C to ground hold bank 0 selected at boot and the problem
 *  disappears. They are not optional.
 *
 *  BOARD    Teensy 4.1
 *  OPTIMISE Faster
 *  THEN     Serial Monitor at 115200. It starts by itself.
 * ===========================================================================================
 */

#include <Arduino.h>

/* ===========================================================================================
 *  THE DECODER DELAY, AND THE ONE REGISTER THAT PAYS FOR IT
 * ===========================================================================================
 *
 *  The decoder sits only in the chip-select path. SCLK goes straight from the Teensy to every chip,
 *  so whatever the decoder takes to propagate arrives as CE# being LATE relative to the clock.
 *
 *  Teensyduino sets the chip-select setup time to ONE serial clock:
 *
 *      cores/teensy4/startup.c:497
 *      FLEXSPI2_FLSHA1CR1 = FLEXSPI_FLSHCR1_CSINTERVAL(0)
 *                         | FLEXSPI_FLSHCR1_TCSH(1) | FLEXSPI_FLSHCR1_TCSS(1);
 *
 *  One clock at 88 MHz is 11.4 ns. A 74HC138 at 3.3 V takes about 30 ns and a 74AHC138 about 12,
 *  so the HC part misses by roughly 19 ns and the first clock of every burst lands while no chip is
 *  listening. That is the whole reason the earlier advice was "buy the LVC part".
 *
 *  It is also unnecessary, because TCSS is a five-bit field and costs almost nothing to raise.
 *
 *      TCSS(4) at 88 MHz = 45.5 ns of setup, which covers an HC part with margin.
 *
 *  A 256-byte burst is 512 clocks at four bits a clock. Going from TCSS(1) to TCSS(4), and TCSH with
 *  it, adds six clocks to about 532, so the cost is a little over one percent of bandwidth. Against
 *  dropping the bus clock to 33 MHz to make TCSS(1) work, which would cost 63%, it is not a close
 *  call.
 *
 *  Set CS_SETUP_CYCLES to 1 to put the stock timing back and watch the bank fail, which is worth
 *  doing once: it is the difference between believing this and knowing it.
 * ======================================================================================== */

#define CS_SETUP_CYCLES 4

static void psram_cs_timing(uint8_t cycles)
{
    /* FLSHxCR1 is sampled at the start of a sequence, so the module has to be idle. Disable, write,
     * re-enable, reset. Doing it without MDIS sometimes works and sometimes corrupts the transfer in
     * flight, which is the worst of both. */
    FLEXSPI2_MCR0 |= FLEXSPI_MCR0_MDIS;
    const uint32_t cr1 = FLEXSPI_FLSHCR1_CSINTERVAL(0)
                       | FLEXSPI_FLSHCR1_TCSH(cycles) | FLEXSPI_FLSHCR1_TCSS(cycles);
    FLEXSPI2_FLSHA1CR1 = cr1;     /* SS0, the chip with no decoder in front of it */
    FLEXSPI2_FLSHA2CR1 = cr1;     /* SS1, the banked side, which is the one that needs it */
    FLEXSPI2_MCR0 &= ~FLEXSPI_MCR0_MDIS;
    FLEXSPI2_MCR0 |= FLEXSPI_MCR0_SWRESET;
    while (FLEXSPI2_MCR0 & FLEXSPI_MCR0_SWRESET) { }
}

/* 74HC138 address inputs. Any three free digital pins; these avoid Serial1 and the LED. */
#define PIN_A   2
#define PIN_B   3
#define PIN_C   4

/* Five banks: the chip already on SS1 is bank 0 and four new ones are banks 1 to 4.
 *
 * That is 8 MB on SS0 plus 40 MB of bank, so 48 MB of storage a Teensy and 36 of the 50 chips
 * across nine boards, with 14 spare. It is one chip and seven solder joints less work per
 * board than six banks.
 *
 * The cost, stated plainly: 48 MB is BELOW the 50.1 MB a layer of this model needs, so a
 * Teensy can no longer hold a whole layer. That costs nothing in practice, because the placer
 * already refused to give it one -- a Teensy reads a layer at 18.2 MB/s against a Zynq's 2290,
 * which made the slowest pipeline stage 6.3x worse. What 48 MB still does: sixteen experts of a
 * mixture-of-experts model at 2.92 MB each, a pipeline stage you can bring up without a Zynq,
 * or the slow half of a KV cache. */
#define BANKS   5                       /* chips wired to decoder outputs Y0..Y4 */

/* EXTMEM starts at 0x70000000. The chip on SS0 is the first 8 MB; the bank appears as the second. */
#define BANK_BASE   ((volatile uint32_t *)0x70800000u)
#define BANK_BYTES  (8u * 1024u * 1024u)
#define BANK_WORDS  (BANK_BYTES / 4u)

extern "C" uint8_t external_psram_size;

static uint32_t g_flush_us = 0;         /* measured cost of one bank switch */

static void cyccnt_begin()
{
    ARM_DEMCR    |= ARM_DEMCR_TRCENA;
    ARM_DWT_CTRL |= ARM_DWT_CTRL_CYCCNTENA;
}

/* ---------------------------------------------------------------------------------------------
 *  the bank switch
 * ------------------------------------------------------------------------------------------ */

/* Clean and invalidate the whole banked window, then change the selection.
 *
 * Clean is needed because writes may still be sitting dirty in cache and would otherwise be written
 * to the NEW chip. Invalidate is needed because reads would otherwise be served from the OLD chip's
 * lines. arm_dcache_flush_delete does both, which is why it is the right call here and why
 * arm_dcache_flush alone would be a subtle bug in one direction and arm_dcache_delete a subtle bug in
 * the other. */
static void psram_bank_select(uint8_t bank)
{
    arm_dcache_flush_delete((void *)BANK_BASE, BANK_BYTES);

    digitalWriteFast(PIN_A, (bank >> 0) & 1);
    digitalWriteFast(PIN_B, (bank >> 1) & 1);
    digitalWriteFast(PIN_C, (bank >> 2) & 1);

    /* The decoder needs its propagation delay before the next FlexSPI2 transaction pulls SS1 low.
     * A 74HC138 at 3.3 V takes about 30 ns; one clock at 600 MHz is 1.7 ns. The barrier orders the
     * GPIO write against what follows, and the few cycles after it cover the part. */
    __asm__ volatile("dsb" ::: "memory");
    for (volatile int i = 0; i < 40; i++) { }
}

/* ---------------------------------------------------------------------------------------------
 *  deterministic content, different per bank
 * ------------------------------------------------------------------------------------------ */

/* A pattern that depends on the bank number AND the address, so a wrong bank and a wrong offset are
 * distinguishable in the failure report. A constant per bank would only catch the first error. */
static inline uint32_t expect(uint8_t bank, uint32_t i)
{
    uint32_t s = 0x9E3779B9u ^ ((uint32_t)(bank + 1) * 0x85EBCA6Bu) ^ (i * 0xC2B2AE35u);
    s ^= s >> 15;
    s *= 0x2545F491u;
    s ^= s >> 13;
    return s;
}

/* Only a slice of each chip is written, because writing 8 MB six times over QSPI takes minutes and
 * finds nothing the slice misses. The slice is spread across the whole address range rather than
 * packed at the start, so a stuck high address bit is still caught. */
#define PROBE_POINTS  4096u
#define PROBE_STRIDE  (BANK_WORDS / PROBE_POINTS)

static void fill_bank(uint8_t bank)
{
    psram_bank_select(bank);
    for (uint32_t p = 0; p < PROBE_POINTS; p++) {
        const uint32_t i = p * PROBE_STRIDE;
        BANK_BASE[i] = expect(bank, i);
    }
}

static uint32_t check_bank(uint8_t bank, uint32_t *first_bad_i, uint32_t *got, uint32_t *want)
{
    psram_bank_select(bank);
    uint32_t bad = 0;
    for (uint32_t p = 0; p < PROBE_POINTS; p++) {
        const uint32_t i = p * PROBE_STRIDE;
        const uint32_t v = BANK_BASE[i];
        const uint32_t w = expect(bank, i);
        if (v != w) {
            if (!bad) { *first_bad_i = i; *got = v; *want = w; }
            bad++;
        }
    }
    return bad;
}

/* If the decoder is dead, every bank holds whatever the LAST fill wrote. Naming that explicitly turns
 * a wall of mismatches into a diagnosis. */
static uint8_t looks_like(uint32_t i, uint32_t v)
{
    for (uint8_t b = 0; b < BANKS; b++) if (expect(b, i) == v) return b;
    return 0xFF;
}

/* ---------------------------------------------------------------------------------------------
 *  bandwidth
 * ------------------------------------------------------------------------------------------ */

/* Cold read: the cache is emptied first, so every word crosses FlexSPI2. A real weight sweep walks
 * memory once and never comes back, so this is the honest figure and the warm one is a curiosity. */
static float read_mbs(uint32_t words)
{
    arm_dcache_flush_delete((void *)BANK_BASE, words * 4u);
    const uint32_t c0 = ARM_DWT_CYCCNT;
    uint32_t sum = 0;
    for (uint32_t i = 0; i < words; i++) sum += BANK_BASE[i];
    const uint32_t c1 = ARM_DWT_CYCCNT;
    /* The sum has to be consumed or the loop is dead code. An earlier version of bench_stream
     * accumulated into a volatile instead, which added a store per iteration and reported reads at a
     * third of their real speed. */
    if (sum == 0xDEADBEEFu) Serial.print(' ');
    const float secs = (float)(c1 - c0) / (float)F_CPU_ACTUAL;
    return (float)(words * 4u) / secs / 1048576.0f;
}

/* ---------------------------------------------------------------------------------------------
 *  the test itself
 * ------------------------------------------------------------------------------------------ */

void setup()
{
    pinMode(PIN_A, OUTPUT);
    pinMode(PIN_B, OUTPUT);
    pinMode(PIN_C, OUTPUT);
    digitalWriteFast(PIN_A, 0);
    digitalWriteFast(PIN_B, 0);
    digitalWriteFast(PIN_C, 0);

    cyccnt_begin();
    Serial.begin(115200);
    while (!Serial && millis() < 4000) { }

    Serial.println();
    Serial.println(F("=============================================================="));
    psram_cs_timing(CS_SETUP_CYCLES);

    Serial.println(F("  PSRAM bank: five chips behind one chip select"));
    Serial.print(F("  chip-select setup raised to "));
    Serial.print(CS_SETUP_CYCLES);
    Serial.print(F(" clocks = "));
    Serial.print(CS_SETUP_CYCLES * 1000.0f / 88.0f, 1);
    Serial.println(F(" ns, which covers a 74HC138's 30 ns"));
    Serial.println(F("=============================================================="));
    Serial.print(F("  Teensyduino found "));
    Serial.print(external_psram_size);
    Serial.println(F(" MB on the QSPI bus"));

    if (external_psram_size < 16) {
        Serial.println(F("  STOP. 16 MB expected: 8 for the chip on SS0 and 8 for the bank on SS1."));
        Serial.println(F("  Seeing less means SS1 answered nothing at boot. In order of likelihood:"));
        Serial.println(F("    - the 10k pulldowns on A, B and C are missing, so the decoder selected"));
        Serial.println(F("      an unconnected output and no chip was there to answer"));
        Serial.println(F("    - the decoder's G2A enable is not on the footprint-2 chip-select pad"));
        Serial.println(F("    - G1 is not at 3.3 V, or G2B is not at ground"));
        Serial.println(F("    - a solder joint on the stack's shared pins"));
        Serial.println(F("  Meter the decoder outputs before reflowing anything."));
        return;
    }

    /* ---- cost of a switch, measured before it is relied on ------------------------------- */
    {
        const uint32_t c0 = ARM_DWT_CYCCNT;
        psram_bank_select(1);
        psram_bank_select(0);
        const uint32_t c1 = ARM_DWT_CYCCNT;
        g_flush_us = (uint32_t)((float)(c1 - c0) / 2.0f / ((float)F_CPU_ACTUAL / 1e6f));
        Serial.print(F("\n  one bank switch costs "));
        Serial.print(g_flush_us);
        Serial.println(F(" us, almost all of it the 8 MB cache flush"));
        Serial.println(F("  so switch rarely and read a lot: a bank holds a layer, you select it once"));
    }

    /* ---- write every bank BEFORE reading any of them ------------------------------------- */
    Serial.print(F("\n  writing all "));
    Serial.print(BANKS);
    Serial.print(F(" banks first ("));
    Serial.print(PROBE_POINTS);
    Serial.println(F(" points each, spread across the full 8 MB)"));
    for (uint8_t b = 0; b < BANKS; b++) {
        fill_bank(b);
        Serial.print('.');
    }
    Serial.println(F(" done"));
    Serial.println(F("  nothing has been read yet, which is what makes the next step mean something"));

    /* ---- now read them back -------------------------------------------------------------- */
    Serial.println(F("\n  bank   result"));
    uint8_t passed = 0;
    uint8_t aliased_to = 0xFF;
    for (uint8_t b = 0; b < BANKS; b++) {
        uint32_t i = 0, got = 0, want = 0;
        const uint32_t bad = check_bank(b, &i, &got, &want);
        Serial.print(F("  "));
        Serial.print(b);
        Serial.print(F("      "));
        if (!bad) {
            Serial.println(F("PASS"));
            passed++;
        } else {
            Serial.print(bad);
            Serial.print(F(" of "));
            Serial.print(PROBE_POINTS);
            Serial.print(F(" wrong; first at word "));
            Serial.print(i);
            Serial.print(F(" got 0x"));
            Serial.print(got, HEX);
            Serial.print(F(" want 0x"));
            Serial.print(want, HEX);
            const uint8_t src = looks_like(i, got);
            if (src != 0xFF) {
                Serial.print(F("  <- that is BANK "));
                Serial.print(src);
                Serial.print(F("'s data"));
                aliased_to = src;
            }
            Serial.println();
        }
    }

    Serial.print(F("\n  "));
    Serial.print(passed);
    Serial.print(F(" of "));
    Serial.print(BANKS);
    Serial.println(F(" banks hold their own data"));

    if (passed == BANKS) {
        Serial.print(F("  SIX REAL CHIPS behind one chip select. With the onboard chip that is "));
        Serial.print((BANKS + 1) * 8);
        Serial.println(F(" MB on one Teensy."));
    } else if (aliased_to != 0xFF) {
        Serial.println(F("  EVERY BANK IS THE SAME PHYSICAL CHIP. The decoder is not switching, so all"));
        Serial.println(F("  six writes landed in one place and the last one won. Check that A, B and C"));
        Serial.println(F("  reach the decoder and that exactly one Y output goes low for each address."));
        Serial.println(F("  This is the failure a write-then-read-immediately test would have passed."));
    } else {
        Serial.println(F("  Mixed failures point at the shared pins rather than the decoder: SCK, the"));
        Serial.println(F("  four data lines, or a missing 100 nF across a chip's power pins. Fit the"));
        Serial.println(F("  22 ohm series resistor on SCK if it is not there -- six loads on one clock"));
        Serial.println(F("  line rings, and ringing shows up as a few wrong bytes, not a dead bank."));
    }

    /* ---- the soak, which is the test that matters on flying wires ------------------------ */

    /* ONE CLEAN PASS PROVES NOTHING ON HAND-WIRED QSPI.
     *
     * The failure mode of long wires at 88 MHz is not a dead bank, it is a handful of wrong bytes
     * every few thousand transactions. A single pattern pass has a good chance of missing that
     * entirely, and then the bank gets trusted and a model quietly produces slightly wrong numbers
     * forever.
     *
     * So the whole write-all-then-read-all cycle runs many times and the errors are counted per bank.
     * What the counts look like tells you WHICH problem you have, and the two have completely
     * different fixes:
     *
     *   every pass clean                 the wiring is good enough at this clock
     *   a few errors, different places   SIGNAL INTEGRITY. Shorten wires, add the 22 ohm on SCLK,
     *     each time, some passes clean   run a ground wire alongside the bundle, or drop the clock
     *   same error every pass            a WIRING MISTAKE or a cold joint. Not a speed problem, and
     *     in the same place              no amount of slowing down will fix it
     *   one bank always wrong            that chip's leg 1, or its decoder output
     */
    {
        const uint8_t PASSES = 25;
        uint32_t err[BANKS];
        uint8_t  dirty_passes = 0;
        for (uint8_t b = 0; b < BANKS; b++) err[b] = 0;

        Serial.print(F("\n  soak: "));
        Serial.print(PASSES);
        Serial.println(F(" full write-all-then-read-all cycles"));
        Serial.println(F("  one clean pass means nothing on flying wires; intermittent is the danger"));

        for (uint8_t p = 0; p < PASSES; p++) {
            for (uint8_t b = 0; b < BANKS; b++) fill_bank(b);
            uint32_t this_pass = 0;
            for (uint8_t b = 0; b < BANKS; b++) {
                uint32_t i = 0, got = 0, want = 0;
                const uint32_t bad = check_bank(b, &i, &got, &want);
                err[b] += bad;
                this_pass += bad;
            }
            if (this_pass) dirty_passes++;
            Serial.print(this_pass ? 'x' : '.');
        }
        Serial.println();

        uint32_t total = 0;
        for (uint8_t b = 0; b < BANKS; b++) total += err[b];

        Serial.print(F("  "));
        Serial.print(dirty_passes);
        Serial.print(F(" of "));
        Serial.print(PASSES);
        Serial.print(F(" passes had an error, "));
        Serial.print(total);
        Serial.print(F(" wrong words out of "));
        Serial.println((uint32_t)PASSES * BANKS * PROBE_POINTS);

        if (!total) {
            Serial.println(F("  CLEAN. The wiring holds at this clock. This is the result you want"));
            Serial.println(F("  before you trust a byte of it to a model."));
        } else if (dirty_passes == PASSES && total == err[0] * BANKS) {
            Serial.println(F("  EVERY PASS, EVERY BANK. That is a wiring mistake, not a speed"));
            Serial.println(F("  problem. Slowing the clock will not help. Check the shared legs."));
        } else if (dirty_passes < PASSES) {
            Serial.println(F("  INTERMITTENT, which is signal integrity. In order of what helps most:"));
            Serial.println(F("    1. shorter wires. Under 50 mm beats anything else you can do"));
            Serial.println(F("    2. a ground wire running alongside the bundle, not just one at the end"));
            Serial.println(F("    3. the 22 ohm resistor in SCLK, if it is not fitted"));
            Serial.println(F("    4. the 100 nF at each chip, if any are missing"));
            Serial.println(F("    5. last resort, halve the FlexSPI2 clock. Costs half the bandwidth"));
            Serial.println(F("       and is still worth it, because capacity is what this buys."));
        } else {
            Serial.println(F("  per-bank counts tell you which chip:"));
            for (uint8_t b = 0; b < BANKS; b++) {
                Serial.print(F("    bank "));
                Serial.print(b);
                Serial.print(F(": "));
                Serial.println(err[b]);
            }
        }
    }

    /* ---- what it costs ------------------------------------------------------------------- */
    Serial.println(F("\n  read bandwidth, cold, bank 0:"));
    psram_bank_select(0);
    const uint32_t sizes[] = { 64u * 1024u, 1024u * 1024u, 4096u * 1024u };
    for (uint8_t s = 0; s < 3; s++) {
        const float mbs = read_mbs(sizes[s] / 4u);
        Serial.print(F("    "));
        Serial.print(sizes[s] / 1024u);
        Serial.print(F(" KB   "));
        Serial.print(mbs, 1);
        Serial.println(F(" MB/s"));
    }

    /* The number that matters for inference: streaming a whole 8 MB bank, switch included. A 49.2 MB
     * transformer layer does not fit one chip, so a layer means several switches, and the flush is
     * charged against every one of them. */
    {
        const float mbs = read_mbs(BANK_WORDS);
        const float with_switch = 8.0f / ((8.0f / mbs) + (float)g_flush_us / 1e6f);
        Serial.print(F("\n  whole 8 MB bank: "));
        Serial.print(mbs, 1);
        Serial.print(F(" MB/s, or "));
        Serial.print(with_switch, 1);
        Serial.println(F(" MB/s counting the switch"));
        Serial.print(F("  a 49.2 MB layer spans 7 banks and would take "));
        Serial.print(49.2f / with_switch, 2);
        Serial.println(F(" s to read once"));
        Serial.println(F("  Capacity is what this buys. Speed is what the FPGA buys."));
    }
    Serial.println();
}

void loop() { }
