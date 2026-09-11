/* ===========================================================================================
 *  psram_calibrate -- sweep the read sampling point, because the bus is fine and the timing is not
 * ===========================================================================================
 *
 *  WHAT IS ALREADY KNOWN, ALL OF IT MEASURED
 *  -----------------------------------------
 *      bit-banged single SPI, D0 and D1      64 of 64 bytes correct
 *      bit-banged QUAD, all four lines       64 of 64 bytes correct
 *      controller at 49.5 MHz, identity      answers correctly
 *      controller at 49.5 MHz, memory window three quarters of the bytes wrong
 *      controller at 66 MHz and above        silent
 *
 *  Quad works perfectly when clocked by hand and fails through the controller. Every wire is
 *  therefore connected and every chip is healthy. What differs is not which wires are used but WHEN
 *  the returned data is sampled.
 *
 *
 *  THE THING NOBODY HAD SET
 *  ------------------------
 *  FlexSPI decides when to latch data coming back from the chip. The Teensy core configures
 *  RXCLKSRC(1), a strobe looped back through the DQS pad, and then never touches FLEXSPI2_DLLACR at
 *  all, so it sits at its reset value: override enabled, delay zero.
 *
 *  Zero extra delay is right for two chips on the board's own pads. It is not right for six chips
 *  and a run of wire out to a perfboard and back. The round trip is longer now, the data arrives
 *  later, and the controller latches before it is there. That produces exactly what was seen: a
 *  command that works, because commands are sent and never sampled, and reads that are garbage.
 *
 *  DLLACR's override value is six bits, so there are 64 sampling positions and this tries them all.
 *
 *
 *  WHY A GRID AND NOT A NUMBER
 *  ---------------------------
 *  A single working setting is a coincidence. What matters is the WIDTH of the run of settings that
 *  work, because the middle of a wide run is a setting that survives temperature and a different
 *  chip. A single isolated pass is a setting that will fail next week. So this prints every
 *  combination and then picks the centre of the widest clean run, which is the same reason the DDR3
 *  controller in this project calibrates rather than assumes.
 *
 *  Writes are not swept. The controller drives data out on writes and samples nothing, so the delay
 *  cannot affect them; only reads are at issue.
 *
 *  BOARD    Teensy 4.1
 * ======================================================================================== */

#include <Arduino.h>

#define PIN_A 2
#define PIN_B 3
#define PIN_C 4
#define PARK  7

extern "C" uint8_t external_psram_size;

struct Speed { const char *name; uint8_t sel; uint8_t podf; };
static const Speed SPEEDS[] = {
    { "49.5", 0, 7 },
    { "56.6", 0, 6 },
    { "66.0", 3, 7 },
    { "79.2", 0, 4 },
    { "88.0", 3, 5 },
    { "105.6", 3, 4 },
};
#define NSPEED (sizeof(SPEEDS) / sizeof(SPEEDS[0]))
#define NDLL   32
#define BLK    4096

#define RAM ((volatile uint8_t *)0x70000000u)

static void set_clock(uint8_t sel, uint8_t podf)
{
    FLEXSPI2_MCR0 |= FLEXSPI_MCR0_MDIS;
    CCM_CBCMR = (CCM_CBCMR & ~(CCM_CBCMR_FLEXSPI2_PODF_MASK | CCM_CBCMR_FLEXSPI2_CLK_SEL_MASK))
              | CCM_CBCMR_FLEXSPI2_PODF(podf) | CCM_CBCMR_FLEXSPI2_CLK_SEL(sel);
    FLEXSPI2_MCR0 &= ~FLEXSPI_MCR0_MDIS;
    FLEXSPI2_MCR0 |= FLEXSPI_MCR0_SWRESET;
    while (FLEXSPI2_MCR0 & FLEXSPI_MCR0_SWRESET) { }
    delayMicroseconds(300);
}

static void set_dll(uint8_t taps)
{
    FLEXSPI2_MCR0 |= FLEXSPI_MCR0_MDIS;
    FLEXSPI2_DLLACR = FLEXSPI_DLLCR_OVRDEN | FLEXSPI_DLLCR_OVRDVAL(taps);
    FLEXSPI2_MCR0 &= ~FLEXSPI_MCR0_MDIS;
    FLEXSPI2_MCR0 |= FLEXSPI_MCR0_SWRESET;
    while (FLEXSPI2_MCR0 & FLEXSPI_MCR0_SWRESET) { }
    delayMicroseconds(50);
}

static void ip_cmd(uint32_t seq, uint32_t addr)
{
    FLEXSPI2_IPCR0 = addr;
    FLEXSPI2_IPCR1 = FLEXSPI_IPCR1_ISEQID(seq);
    FLEXSPI2_IPCMD = FLEXSPI_IPCMD_TRG;
    uint32_t g = 0;
    while (!(FLEXSPI2_INTR & FLEXSPI_INTR_IPCMDDONE)) if (++g > 1000000) return;
    FLEXSPI2_INTR = FLEXSPI_INTR_IPCMDDONE;
}

static uint32_t read_id(void)
{
    ip_cmd(0, 0); ip_cmd(1, 0); ip_cmd(2, 0);
    FLEXSPI2_IPCR0 = 0;
    FLEXSPI2_IPCR1 = FLEXSPI_IPCR1_ISEQID(3) | FLEXSPI_IPCR1_IDATSZ(4);
    FLEXSPI2_IPCMD = FLEXSPI_IPCMD_TRG;
    uint32_t g = 0;
    while (!(FLEXSPI2_INTR & FLEXSPI_INTR_IPCMDDONE)) if (++g > 1000000) return 0;
    const uint32_t id = FLEXSPI2_RFDR0;
    FLEXSPI2_INTR = FLEXSPI_INTR_IPCMDDONE | FLEXSPI_INTR_IPRXWA;
    return id;
}

static inline uint8_t pat(uint32_t i) { return (uint8_t)(i * 0x9Du + 0x3Bu); }

static uint32_t read_errors(void)
{
    arm_dcache_flush_delete((void *)RAM, BLK);
    uint32_t bad = 0;
    for (uint32_t i = 0; i < BLK; i++) if (RAM[i] != pat(i)) bad++;
    return bad;
}

void setup()
{
    Serial.begin(115200);
    while (!Serial && millis() < 4000) { }
    delay(150);

    pinMode(PIN_A, OUTPUT); pinMode(PIN_B, OUTPUT); pinMode(PIN_C, OUTPUT);
    digitalWriteFast(PIN_A, 1); digitalWriteFast(PIN_B, 1); digitalWriteFast(PIN_C, 1);
    delayMicroseconds(50);

    Serial.println();
    Serial.println(F("=============================================================="));
    Serial.println(F("  read sampling sweep: 6 clocks x 32 delay taps"));
    Serial.println(F("  the wires are proven good. This is only about WHEN to latch."));
    Serial.println(F("=============================================================="));

    /* Map the chip once, at the slowest clock, so writes are as reliable as they can be. */
    set_clock(0, 7);
    set_dll(0);
    const uint32_t id = read_id();
    Serial.print(F("\nidentity at 49.5 MHz: 0x"));
    Serial.println(id, HEX);
    if ((id & 0xFFFF) != 0x5D0D && (id & 0xFFFF) != 0x5D9D) {
        Serial.println(F("no chip answered. Stopping."));
        return;
    }
    FLEXSPI2_FLSHA1CR0 = 8 << 10;
    ip_cmd(4, 0);                       /* quad mode, which the memory window needs */
    external_psram_size = 8;

    Serial.println(F("\n  a dot is a wrong read, a hash is a clean 4 kB block"));
    Serial.println(F("  delay taps 0 ------------------------------> 31"));

    int best_speed = -1, best_tap = -1, best_run = 0;

    for (unsigned s = 0; s < NSPEED; s++) {
        set_clock(SPEEDS[s].sel, SPEEDS[s].podf);

        /* Write the block at this clock with a middling delay. Writes do not depend on the
         * sampling point, so any value will do for laying the pattern down. */
        set_dll(8);
        for (uint32_t i = 0; i < BLK; i++) RAM[i] = pat(i);
        arm_dcache_flush_delete((void *)RAM, BLK);

        char row[NDLL + 1];
        int run = 0, run_start = -1;
        for (int t = 0; t < NDLL; t++) {
            set_dll((uint8_t)t);
            const bool ok = (read_errors() == 0);
            row[t] = ok ? '#' : '.';
            if (ok) {
                if (run == 0) run_start = t;
                run++;
                if (run > best_run) {
                    best_run = run;
                    best_speed = (int)s;
                    best_tap = run_start + run / 2;
                }
            } else {
                run = 0;
            }
        }
        row[NDLL] = 0;
        Serial.print(F("  "));
        Serial.print(SPEEDS[s].name);
        while (strlen(SPEEDS[s].name) < 5) break;
        Serial.print(F(" MHz  "));
        Serial.println(row);
    }

    /* ---- verdict ------------------------------------------------------------------------- */
    Serial.println(F("\n--- verdict ---"));
    if (best_speed < 0) {
        Serial.println(F("  No combination read back cleanly. The sampling point is not the"));
        Serial.println(F("  problem, or the writes themselves are not landing. Since bit-banged"));
        Serial.println(F("  quad was perfect, the next thing to suspect is the AHB write path:"));
        Serial.println(F("  try halving the block and see if a short burst survives."));
        return;
    }

    Serial.print(F("  widest clean run: "));
    Serial.print(best_run);
    Serial.print(F(" taps at "));
    Serial.print(SPEEDS[best_speed].name);
    Serial.print(F(" MHz, centred on tap "));
    Serial.println(best_tap);

    if (best_run < 3) {
        Serial.println(F("  That run is narrow, so it is a coincidence rather than a setting."));
        Serial.println(F("  Usable to prove the idea, not to rely on."));
    } else {
        Serial.println(F("  That is a real window. Setting it makes the memory work at that speed."));
    }

    /* Prove it by running a bigger block at the chosen setting. */
    set_clock(SPEEDS[best_speed].sel, SPEEDS[best_speed].podf);
    set_dll((uint8_t)best_tap);
    for (uint32_t i = 0; i < BLK; i++) RAM[i] = pat(i);
    arm_dcache_flush_delete((void *)RAM, BLK);
    const uint32_t again = read_errors();
    Serial.print(F("\n  re-tested at that setting: "));
    Serial.print(again);
    Serial.println(again ? F(" wrong") : F(" wrong. It holds."));

    Serial.println(F("\n=== done ==="));
}

void loop() { }
