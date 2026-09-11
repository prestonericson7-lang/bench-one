/* ===========================================================================================
 *  psram_clock_sweep -- find the speed at which six chips on a hand-wired bus still answer
 * ===========================================================================================
 *
 *  WHAT THE BIT-BANG PROBE ESTABLISHED
 *  -----------------------------------
 *  All six chips are present, powered and correctly wired. Every one of them answered command 0x9F
 *  with 0D 5D, which is a real ESP-PSRAM64H signature and cannot appear on a broken bus. The five
 *  banked chips answered through the decoder on Y0 to Y4, and the onboard chip answered on its own
 *  with the decoder parked on the unconnected Y7.
 *
 *  The soldering is good. That is settled and is not what this is testing.
 *
 *  The probe reported three data lines "shorted to 3.3 V" and that was MY TEST BEING WRONG, not the
 *  board. An internal pulldown of about 100 k is no match for six chips' worth of input leakage and
 *  the pad configuration the core had already left behind. The proof it was a false alarm is the
 *  identity read itself: a line shorted to 3.3 V cannot carry a zero bit, and those replies are full
 *  of zero bits.
 *
 *
 *  SO WHY DOES THE CONTROLLER STILL SEE NOTHING
 *  --------------------------------------------
 *  Because bit-banging runs at roughly 250 kHz and the controller runs at 105.6 MHz, and that is the
 *  only difference left between a bus that works and a bus that does not.
 *
 *  Six chips hang off these four data lines now, each contributing a few picofarads, plus every
 *  centimetre of wire out to the perfboard and back. Capacitance and stub length do nothing at
 *  250 kHz and are decisive at 105 MHz. This is the same reason the wiring notes ask for stubs under
 *  50 mm: not because delay matters, but because the bus has to settle inside one clock.
 *
 *  If that is right, there is a speed at which it starts working, and this finds it. FlexSPI2 can be
 *  clocked from 49.5 MHz up, so the sweep runs from the bottom.
 *
 *
 *  IT ALSO SEPARATES THE TWO FAULTS
 *  --------------------------------
 *  Each speed is tried twice: once with the decoder parked on Y7, where only the onboard chip can
 *  answer, and once on Y0, where the onboard chip and a banked chip answer together. Speed and
 *  contention then show up as different columns instead of one confusing number.
 *
 *  BOARD    Teensy 4.1
 * ======================================================================================== */

#include <Arduino.h>

#define PIN_A 2
#define PIN_B 3
#define PIN_C 4
#define A_BIT (1u << 4)
#define B_BIT (1u << 5)
#define C_BIT (1u << 6)
#define PARK  7

extern "C" uint8_t external_psram_size;

/* Park the decoder on a bare output before the core probes the bus. */
FLASHMEM void startup_early_hook(void)
{
    IOMUXC_SW_MUX_CTL_PAD_GPIO_EMC_04 = 5;
    IOMUXC_SW_MUX_CTL_PAD_GPIO_EMC_05 = 5;
    IOMUXC_SW_MUX_CTL_PAD_GPIO_EMC_06 = 5;
    IOMUXC_SW_PAD_CTL_PAD_GPIO_EMC_04 = IOMUXC_PAD_DSE(7);
    IOMUXC_SW_PAD_CTL_PAD_GPIO_EMC_05 = IOMUXC_PAD_DSE(7);
    IOMUXC_SW_PAD_CTL_PAD_GPIO_EMC_06 = IOMUXC_PAD_DSE(7);
    GPIO4_GDIR |= (A_BIT | B_BIT | C_BIT);
    GPIO4_DR_SET = (A_BIT | B_BIT | C_BIT);      /* 111 = Y7, which is wired to nothing */
}

/* ---- the clock table ---------------------------------------------------------------------
 * FlexSPI2's source is chosen by CLK_SEL and then divided by PODF+1. The four sources are
 * 396, 720, 664.8 and 528 MHz for SEL 0 to 3. The slowest the controller can be run is
 * 396/8 = 49.5 MHz, so that is the bottom of this sweep. The core's own default is 105.6.
 * ---------------------------------------------------------------------------------------- */
struct Speed { const char *name; uint8_t sel; uint8_t podf; };
static const Speed SPEEDS[] = {
    { " 49.5", 0, 7 },
    { " 56.6", 0, 6 },
    { " 66.0", 3, 7 },
    { " 79.2", 0, 4 },
    { " 88.0", 3, 5 },
    { " 99.0", 0, 3 },
    { "105.6", 3, 4 },      /* what the core used, and what failed */
};
#define NSPEED (sizeof(SPEEDS) / sizeof(SPEEDS[0]))

static void set_flexspi2_clock(uint8_t sel, uint8_t podf)
{
    FLEXSPI2_MCR0 |= FLEXSPI_MCR0_MDIS;
    CCM_CBCMR = (CCM_CBCMR & ~(CCM_CBCMR_FLEXSPI2_PODF_MASK | CCM_CBCMR_FLEXSPI2_CLK_SEL_MASK))
              | CCM_CBCMR_FLEXSPI2_PODF(podf) | CCM_CBCMR_FLEXSPI2_CLK_SEL(sel);
    FLEXSPI2_MCR0 &= ~FLEXSPI_MCR0_MDIS;
    FLEXSPI2_MCR0 |= FLEXSPI_MCR0_SWRESET;
    while (FLEXSPI2_MCR0 & FLEXSPI_MCR0_SWRESET) { }
    delayMicroseconds(200);
}

/* The LUT the core built is still loaded even though its probe failed, so these sequence
 * numbers still mean what they meant in startup.c. */
static void ip_cmd(uint32_t seq, uint32_t addr)
{
    FLEXSPI2_IPCR0 = addr;
    FLEXSPI2_IPCR1 = FLEXSPI_IPCR1_ISEQID(seq);
    FLEXSPI2_IPCMD = FLEXSPI_IPCMD_TRG;
    uint32_t guard = 0;
    while (!(FLEXSPI2_INTR & FLEXSPI_INTR_IPCMDDONE)) if (++guard > 2000000) return;
    FLEXSPI2_INTR = FLEXSPI_INTR_IPCMDDONE;
}

static uint32_t read_id(uint32_t addr)
{
    ip_cmd(0, addr);          /* exit quad mode */
    ip_cmd(1, addr);          /* reset enable   */
    ip_cmd(2, addr);          /* reset          */
    FLEXSPI2_IPCR0 = addr;
    FLEXSPI2_IPCR1 = FLEXSPI_IPCR1_ISEQID(3) | FLEXSPI_IPCR1_IDATSZ(4);
    FLEXSPI2_IPCMD = FLEXSPI_IPCMD_TRG;
    uint32_t guard = 0;
    while (!(FLEXSPI2_INTR & FLEXSPI_INTR_IPCMDDONE)) if (++guard > 2000000) return 0xDEADBEEF;
    const uint32_t id = FLEXSPI2_RFDR0;
    FLEXSPI2_INTR = FLEXSPI_INTR_IPCMDDONE | FLEXSPI_INTR_IPRXWA;
    return id;
}

static void pick(uint8_t n)
{
    digitalWriteFast(PIN_A, (n >> 0) & 1);
    digitalWriteFast(PIN_B, (n >> 1) & 1);
    digitalWriteFast(PIN_C, (n >> 2) & 1);
    delayMicroseconds(20);
}

static bool good(uint32_t id) { return (id & 0xFFFF) == 0x5D0D || (id & 0xFFFF) == 0x5D9D; }

void setup()
{
    Serial.begin(115200);
    while (!Serial && millis() < 4000) { }
    delay(150);

    Serial.println();
    Serial.println(F("=============================================================="));
    Serial.println(F("  how fast can this bus actually go, with six chips on it"));
    Serial.println(F("=============================================================="));

    /* ---- did the early hook take effect at all? ------------------------------------------ */
    const uint32_t dr = GPIO4_DR;
    const bool parked = (dr & A_BIT) && (dr & B_BIT) && (dr & C_BIT);
    Serial.print(F("\n[1] address lines at boot: A="));
    Serial.print((dr & A_BIT) ? 1 : 0);
    Serial.print(F(" B="));  Serial.print((dr & B_BIT) ? 1 : 0);
    Serial.print(F(" C="));  Serial.print((dr & C_BIT) ? 1 : 0);
    Serial.println(parked ? F("   the early hook DID run, decoder parked on Y7")
                          : F("   the early hook did NOT take. That is the fault."));

    Serial.print(F("    the core's own probe found "));
    Serial.print(external_psram_size);
    Serial.println(F(" MB at 105.6 MHz"));

    pinMode(PIN_A, OUTPUT); pinMode(PIN_B, OUTPUT); pinMode(PIN_C, OUTPUT);

    /* ---- sweep ---------------------------------------------------------------------------- */
    Serial.println(F("\n[2] identity read at each clock, on the onboard chip alone (Y7)"));
    Serial.println(F("    and with a banked chip answering too (Y0)\n"));
    Serial.println(F("      MHz     Y7 alone            Y0 shared"));
    Serial.println(F("      -----   -----------------   -----------------"));

    int best = -1;
    for (unsigned i = 0; i < NSPEED; i++) {
        set_flexspi2_clock(SPEEDS[i].sel, SPEEDS[i].podf);

        pick(PARK);
        const uint32_t a = read_id(0);
        pick(0);
        const uint32_t b = read_id(0);
        pick(PARK);

        Serial.print(F("      "));
        Serial.print(SPEEDS[i].name);
        Serial.print(F("   "));
        Serial.print(a, HEX);
        Serial.print(good(a) ? F("  OK      ") : F("  --      "));
        Serial.print(F("   "));
        Serial.print(b, HEX);
        Serial.println(good(b) ? F("  OK") : F("  --"));

        if (good(a)) best = (int)i;
    }

    /* ---- what it means -------------------------------------------------------------------- */
    Serial.println(F("\n--- verdict ---"));
    if (best < 0) {
        Serial.println(F("  Nothing answered at ANY speed, yet the bit-bang probe got a clean"));
        Serial.println(F("  signature from every chip. That points at the controller's setup"));
        Serial.println(F("  rather than at the wiring: most likely the chips are still in quad"));
        Serial.println(F("  mode from the earlier attempts and the exit-quad command is not"));
        Serial.println(F("  reaching them. Power the board down fully, then run this again."));
    } else {
        Serial.print(F("  The onboard chip answers up to "));
        Serial.print(SPEEDS[best].name);
        Serial.println(F(" MHz."));
        if (best == NSPEED - 1) {
            Serial.println(F("  That is the full speed the core uses, so the bus is NOT too slow"));
            Serial.println(F("  and the earlier zero was something else."));
        } else {
            Serial.println(F("  It fails above that, which is the six chips' capacitance and the"));
            Serial.println(F("  length of the stubs out to the perfboard. Nothing is broken: the"));
            Serial.println(F("  bus simply cannot settle inside one clock at the stock speed."));
            Serial.println(F("  Setting FlexSPI2 to that speed at boot makes the memory usable."));
        }
    }
    Serial.println(F("\n  Y0 answering as well as Y7 would mean the banked chips are reachable;"));
    Serial.println(F("  Y0 failing where Y7 works is the two-chips-one-select collision."));
    Serial.println(F("\n=== done ==="));
}

void loop() { }
