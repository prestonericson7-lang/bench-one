/* ===========================================================================================
 *  psram_one_cs -- make the board work when the decoder shares a chip select with the onboard chip
 * ===========================================================================================
 *
 *  WHAT HAPPENED
 *  -------------
 *  The chip-select pad of the SECOND QSPI footprint tore off. That pad is the only place Teensy pin
 *  51 exists on the whole board -- on the pinout card every other QSPI signal is marked as appearing
 *  twice, and 48 and 51 are not -- so there is no second place to pick it up.
 *
 *  The decoder's G2A enable was therefore wired to the FIRST footprint's chip-select pad instead,
 *  which is the pad the onboard PSRAM is already using. Both now answer at the same instant: the
 *  onboard chip because that is its own select, and one banked chip because the decoder was enabled
 *  by the same edge. Two devices drive the four data lines together and the bus returns nothing
 *  usable, which is why the bank test reported 0 MB rather than 8.
 *
 *
 *  THE WORKAROUND, AND WHAT IT CANNOT DO
 *  -------------------------------------
 *  The 74LVC138A has eight outputs and only five are wired. Y5, Y6 and Y7 go nowhere.
 *
 *  Park the address lines on one of those and the decoder still enables, still drives one output
 *  low, and that output is connected to nothing. Every banked chip stays deselected, the onboard
 *  chip answers by itself, and the bus is clean.
 *
 *  So this recovers the board and the 8 MB that is already on it. It does NOT recover the five
 *  banked chips, and no amount of code can: reaching any of them means driving Y0 to Y4, and that
 *  edge is the same edge that selects the onboard chip. One select cannot serve two devices.
 *
 *
 *  WHY THIS HAS TO HAPPEN IN startup_early_hook
 *  --------------------------------------------
 *  The core probes the QSPI bus from configure_external_ram(), long before setup() runs. Anything
 *  done in setup() is far too late -- the probe has already failed and external_psram_size is
 *  already 0.
 *
 *  In the core's startup.c, startup_early_hook() is called at line 108 and configure_external_ram()
 *  at line 187. That gap is the whole opportunity. The hook runs before the PLLs are configured and
 *  before .data is copied, so it must be FLASHMEM and must touch nothing but registers -- no
 *  pinMode, no digitalWrite, no globals.
 *
 *  Pins 2, 3, 4 are GPIO_EMC_04, 05 and 06, which are GPIO4 bits 4, 5 and 6. Not GPIO9: that is the
 *  high-speed alias and it only exists once IOMUXC_GPR_GPR27 has been set, which has not happened
 *  this early.
 *
 *
 *  IT ALSO TESTS THE EXPLANATION INSTEAD OF ASSUMING IT
 *  ---------------------------------------------------
 *  Everything above is a theory about someone else's soldering. Section 3 checks it: the same bytes
 *  are read back with the decoder parked on a bare output, and then again with it pointed at a real
 *  chip. If the first is clean and the second is not, the contention is real and on this board. If
 *  BOTH are clean, the theory is wrong, the banked chips are reachable after all, and that is worth
 *  far more than being right.
 *
 *  BOARD    Teensy 4.1
 *  THEN     Serial Monitor. It starts by itself.
 * ======================================================================================== */

#include <Arduino.h>

#define PIN_A 2
#define PIN_B 3
#define PIN_C 4

/* GPIO4 bit positions for pins 2, 3 and 4. */
#define A_BIT (1u << 4)
#define B_BIT (1u << 5)
#define C_BIT (1u << 6)

/* Which decoder output to park on. 5, 6 and 7 are all unwired; 7 is furthest from the ones in use,
 * so a single stuck address line still lands on 5 or 6 rather than on a chip. */
#define PARK 7

extern "C" uint8_t external_psram_size;

/* ===========================================================================================
 *  Runs BEFORE the core probes the QSPI bus. Registers only.
 * ======================================================================================== */
FLASHMEM void startup_early_hook(void)
{
    /* ALT5 is GPIO for the EMC pads. */
    IOMUXC_SW_MUX_CTL_PAD_GPIO_EMC_04 = 5;
    IOMUXC_SW_MUX_CTL_PAD_GPIO_EMC_05 = 5;
    IOMUXC_SW_MUX_CTL_PAD_GPIO_EMC_06 = 5;

    /* Strongest drive: these feed a decoder input and the pad may still be at its reset default,
     * which is a weak driver. */
    IOMUXC_SW_PAD_CTL_PAD_GPIO_EMC_04 = IOMUXC_PAD_DSE(7);
    IOMUXC_SW_PAD_CTL_PAD_GPIO_EMC_05 = IOMUXC_PAD_DSE(7);
    IOMUXC_SW_PAD_CTL_PAD_GPIO_EMC_06 = IOMUXC_PAD_DSE(7);

    GPIO4_GDIR |= (A_BIT | B_BIT | C_BIT);

    /* PARK as three bits. Written this way so changing PARK cannot silently drive the wrong one. */
    uint32_t set = 0, clr = 0;
    ((PARK >> 0) & 1) ? (set |= A_BIT) : (clr |= A_BIT);
    ((PARK >> 1) & 1) ? (set |= B_BIT) : (clr |= B_BIT);
    ((PARK >> 2) & 1) ? (set |= C_BIT) : (clr |= C_BIT);
    GPIO4_DR_SET = set;
    GPIO4_DR_CLEAR = clr;
}

/* ======================================================================================== */

#define RAM_BASE ((volatile uint8_t *)0x70000000u)

static void select_output(uint8_t n)
{
    digitalWriteFast(PIN_A, (n >> 0) & 1);
    digitalWriteFast(PIN_B, (n >> 1) & 1);
    digitalWriteFast(PIN_C, (n >> 2) & 1);
    /* The window is cached and write-back. Without this the processor answers the next read out of
     * cache and the decoder setting makes no observable difference, which would make section 3
     * report a clean pass no matter what the hardware did. */
    arm_dcache_flush_delete((void *)RAM_BASE, 8u * 1024u * 1024u);
    delayMicroseconds(20);
}

static inline uint8_t pat(uint32_t i) { return (uint8_t)(i * 0x9Du + 0x3Bu); }

static uint32_t check(uint32_t bytes)
{
    uint32_t bad = 0;
    for (uint32_t i = 0; i < bytes; i++)
        if (RAM_BASE[i] != pat(i)) bad++;
    return bad;
}

void setup()
{
    Serial.begin(115200);
    while (!Serial && millis() < 4000) { }
    delay(150);

    pinMode(PIN_A, OUTPUT);
    pinMode(PIN_B, OUTPUT);
    pinMode(PIN_C, OUTPUT);
    select_output(PARK);

    Serial.println();
    Serial.println(F("=============================================================="));
    Serial.println(F("  one chip select, shared between the onboard chip and the decoder"));
    Serial.print(F("  address lines parked on Y"));
    Serial.print(PARK);
    Serial.println(F(" before the bus was probed"));
    Serial.println(F("=============================================================="));

    /* ---- 1. did parking it get the onboard chip detected? ------------------------------- */
    Serial.print(F("\n[1] the core found "));
    Serial.print(external_psram_size);
    Serial.println(F(" MB"));

    if (external_psram_size == 0) {
        Serial.println(F("    STILL ZERO, so parking the decoder did not fix it and the"));
        Serial.println(F("    explanation is wrong somewhere. Check, in this order:"));
        Serial.println(F("      - decoder pin 16 to ground should read 3.3 V"));
        Serial.println(F("      - decoder pin 6 (G1) to 3.3 V, pin 5 (G2B) to ground"));
        Serial.println(F("      - with the board running, decoder pins 15, 14, 13, 12 and 11"));
        Serial.println(F("        should ALL be high now. Any one of them low is the fault."));
        Serial.println(F("      - two of D0 to D3 bridged in the new wiring"));
        Serial.println(F("      - a dry joint on the onboard chip itself"));
        return;
    }
    Serial.println(F("    the onboard chip answers on its own. The bus is clean."));

    /* ---- 2. is that 8 MB actually sound? ------------------------------------------------ */
    const uint32_t TEST = 2u * 1024u * 1024u;
    Serial.print(F("\n[2] writing and reading "));
    Serial.print(TEST / 1024u / 1024u);
    Serial.println(F(" MB with the decoder parked"));

    for (uint32_t i = 0; i < TEST; i++) RAM_BASE[i] = pat(i);
    arm_dcache_flush_delete((void *)RAM_BASE, TEST);
    const uint32_t bad_parked = check(TEST);

    Serial.print(F("    wrong bytes: "));
    Serial.println(bad_parked);
    if (bad_parked == 0) Serial.println(F("    the memory is good."));
    else                 Serial.println(F("    NOT clean even parked -- this is a wiring fault, not contention."));

    /* ---- 3. now prove the contention, rather than asserting it -------------------------- *
     * Point the decoder at a real chip and read the SAME bytes back. Nothing is written here:
     * two sets of output drivers fighting is not something to sit in for long, and a few
     * thousand reads is enough to see it.                                                   */
    Serial.println(F("\n[3] pointing the decoder at Y0, which selects a banked chip"));
    Serial.println(F("    reading only, briefly. Nothing is written in this state."));

    select_output(0);
    const uint32_t bad_contended = check(64u * 1024u);
    select_output(PARK);

    Serial.print(F("    wrong bytes in the first 64 kB: "));
    Serial.println(bad_contended);

    /* ---- what it all means, from what was measured -------------------------------------- */
    Serial.println(F("\n--- verdict ---"));
    if (bad_parked == 0 && bad_contended > 0) {
        Serial.println(F("  Confirmed. Clean on a bare output, corrupt on a real one."));
        Serial.println(F("  The onboard chip and a banked chip are driving the bus together."));
        Serial.println(F("  You have 8 MB and it works. The five banked chips cannot be reached"));
        Serial.println(F("  until the first chip select belongs to ONE device:"));
        Serial.println(F("    - remove the onboard chip, and the five become the whole memory, or"));
        Serial.println(F("    - lift only its pin 1 and run that leg to decoder pin 10 (Y5),"));
        Serial.println(F("      which makes it a sixth bank instead of a rival."));
    } else if (bad_parked == 0 && bad_contended == 0) {
        Serial.println(F("  Both clean, so the contention theory is WRONG on this board."));
        Serial.println(F("  Either the decoder is not enabling, or Y0 is not reaching a chip."));
        Serial.println(F("  Meter decoder pin 15 while this runs: it should be low, and if it is"));
        Serial.println(F("  high the enable wire is the thing to look at."));
        Serial.println(F("  If it IS low and reads are still clean, the banked chip on Y0 is not"));
        Serial.println(F("  connected to the data lines -- and that is a much easier fix."));
    } else {
        Serial.println(F("  The memory is not sound even with the decoder parked, so the fault is"));
        Serial.println(F("  in the shared wiring rather than in the chip select. Two of D0 to D3"));
        Serial.println(F("  bridged would do it. Meter every data line against its neighbour."));
    }
    Serial.println(F("\n=== done ==="));
}

void loop() { }
