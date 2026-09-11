/* ===========================================================================================
 *  psram_slow_bus -- bring the memory up at a speed this bus can actually hold
 * ===========================================================================================
 *
 *  WHAT WAS MEASURED, NOT GUESSED
 *  ------------------------------
 *  Sweeping FlexSPI2 across every clock it can generate:
 *
 *      49.5 MHz   answers
 *      56.6 MHz   answers
 *      66.0 MHz   silent
 *      79.2 and above   silent
 *
 *  The wiring is sound -- every one of the six chips returned a correct signature to a bit-banged
 *  identity command. What fails is settling time. Six chips and the run out to the perfboard put far
 *  more capacitance on four data lines than two chips on the board's own pads ever did, and above
 *  about 60 MHz the bus can no longer settle inside a clock. The stock speed is 105.6 MHz, so the
 *  core's probe was never going to see anything.
 *
 *  This runs the bus at 49.5 MHz, the slowest the controller can be set to and comfortably below the
 *  measured edge rather than on it.
 *
 *
 *  WHY THE CORE CANNOT BE TALKED INTO THIS
 *  ---------------------------------------
 *  configure_external_ram() sets the clock to 105.6 MHz itself and then probes, so nothing done
 *  before it survives. startup_middle_hook() runs afterwards, too late for the probe. The
 *  arrangement here is simply to let the core's probe fail, then redo its last few steps at a speed
 *  the hardware can hold: set the clock, identify the chip, map it, switch it to quad.
 *
 *
 *  AND WHY THE EARLY HOOK DID NOTHING LAST TIME
 *  --------------------------------------------
 *  It wrote GPIO4, which was right when it ran. Four lines later the core sets IOMUXC_GPR_GPR27, and
 *  from that moment those pads belong to GPIO9 instead -- a different register that had never been
 *  written, so the pins fell back to zero well before the probe. Writing both instances fixes it. It
 *  is a good example of a change that looks correct, compiles, runs, and is undone by something that
 *  happens forty lines later.
 *
 *  BOARD    Teensy 4.1
 * ======================================================================================== */

#include <Arduino.h>

#define PIN_A 2
#define PIN_B 3
#define PIN_C 4
#define A_BIT (1u << 4)          /* GPIO_EMC_04, bit 4 on both GPIO4 and GPIO9 */
#define B_BIT (1u << 5)
#define C_BIT (1u << 6)
#define PARK  7                  /* a decoder output wired to nothing */

extern "C" uint8_t external_psram_size;

FLASHMEM void startup_early_hook(void)
{
    IOMUXC_SW_MUX_CTL_PAD_GPIO_EMC_04 = 5;
    IOMUXC_SW_MUX_CTL_PAD_GPIO_EMC_05 = 5;
    IOMUXC_SW_MUX_CTL_PAD_GPIO_EMC_06 = 5;
    IOMUXC_SW_PAD_CTL_PAD_GPIO_EMC_04 = IOMUXC_PAD_DSE(7);
    IOMUXC_SW_PAD_CTL_PAD_GPIO_EMC_05 = IOMUXC_PAD_DSE(7);
    IOMUXC_SW_PAD_CTL_PAD_GPIO_EMC_06 = IOMUXC_PAD_DSE(7);

    /* BOTH instances. GPIO4 is live now; GPIO9 takes over at IOMUXC_GPR_GPR27 a few lines later,
     * and whatever GPIO9 holds at that moment is what the pins become. */
    GPIO4_GDIR |= (A_BIT | B_BIT | C_BIT);
    GPIO4_DR_SET = (A_BIT | B_BIT | C_BIT);
    GPIO9_GDIR |= (A_BIT | B_BIT | C_BIT);
    GPIO9_DR_SET = (A_BIT | B_BIT | C_BIT);
}

/* ---- clock -------------------------------------------------------------------------------- */
static void flexspi2_clock(uint8_t sel, uint8_t podf)
{
    FLEXSPI2_MCR0 |= FLEXSPI_MCR0_MDIS;
    CCM_CBCMR = (CCM_CBCMR & ~(CCM_CBCMR_FLEXSPI2_PODF_MASK | CCM_CBCMR_FLEXSPI2_CLK_SEL_MASK))
              | CCM_CBCMR_FLEXSPI2_PODF(podf) | CCM_CBCMR_FLEXSPI2_CLK_SEL(sel);
    FLEXSPI2_MCR0 &= ~FLEXSPI_MCR0_MDIS;
    FLEXSPI2_MCR0 |= FLEXSPI_MCR0_SWRESET;
    while (FLEXSPI2_MCR0 & FLEXSPI_MCR0_SWRESET) { }
    delayMicroseconds(300);
}

static void ip_cmd(uint32_t seq, uint32_t addr)
{
    FLEXSPI2_IPCR0 = addr;
    FLEXSPI2_IPCR1 = FLEXSPI_IPCR1_ISEQID(seq);
    FLEXSPI2_IPCMD = FLEXSPI_IPCMD_TRG;
    uint32_t g = 0;
    while (!(FLEXSPI2_INTR & FLEXSPI_INTR_IPCMDDONE)) if (++g > 2000000) return;
    FLEXSPI2_INTR = FLEXSPI_INTR_IPCMDDONE;
}

static uint32_t read_id(void)
{
    ip_cmd(0, 0); ip_cmd(1, 0); ip_cmd(2, 0);
    FLEXSPI2_IPCR0 = 0;
    FLEXSPI2_IPCR1 = FLEXSPI_IPCR1_ISEQID(3) | FLEXSPI_IPCR1_IDATSZ(4);
    FLEXSPI2_IPCMD = FLEXSPI_IPCMD_TRG;
    uint32_t g = 0;
    while (!(FLEXSPI2_INTR & FLEXSPI_INTR_IPCMDDONE)) if (++g > 2000000) return 0;
    const uint32_t id = FLEXSPI2_RFDR0;
    FLEXSPI2_INTR = FLEXSPI_INTR_IPCMDDONE | FLEXSPI_INTR_IPRXWA;
    return id;
}

#define RAM ((volatile uint8_t *)0x70000000u)
#define CHUNK (64u * 1024u)

static void pick(uint8_t n)
{
    digitalWriteFast(PIN_A, (n >> 0) & 1);
    digitalWriteFast(PIN_B, (n >> 1) & 1);
    digitalWriteFast(PIN_C, (n >> 2) & 1);
    arm_dcache_flush_delete((void *)RAM, CHUNK);
    delayMicroseconds(50);
}

static void fill(uint8_t seed)
{
    for (uint32_t i = 0; i < CHUNK; i++) RAM[i] = (uint8_t)(i * 0x9Du + seed);
    arm_dcache_flush_delete((void *)RAM, CHUNK);
}

static uint32_t verify(uint8_t seed)
{
    arm_dcache_flush_delete((void *)RAM, CHUNK);
    uint32_t bad = 0;
    for (uint32_t i = 0; i < CHUNK; i++)
        if (RAM[i] != (uint8_t)(i * 0x9Du + seed)) bad++;
    return bad;
}

/* ======================================================================================== */

void setup()
{
    Serial.begin(115200);
    while (!Serial && millis() < 4000) { }
    delay(150);

    Serial.println();
    Serial.println(F("=============================================================="));
    Serial.println(F("  bringing the bus up at 49.5 MHz, which it can actually hold"));
    Serial.println(F("=============================================================="));

    const uint32_t dr = GPIO9_DR;
    Serial.print(F("\n[1] address lines at boot: A="));
    Serial.print((dr & A_BIT) ? 1 : 0);
    Serial.print(F(" B=")); Serial.print((dr & B_BIT) ? 1 : 0);
    Serial.print(F(" C=")); Serial.print((dr & C_BIT) ? 1 : 0);
    Serial.println(((dr & A_BIT) && (dr & B_BIT) && (dr & C_BIT))
                   ? F("   parked on Y7, the hook worked this time")
                   : F("   NOT parked, the hook is still being undone"));

    pinMode(PIN_A, OUTPUT); pinMode(PIN_B, OUTPUT); pinMode(PIN_C, OUTPUT);
    digitalWriteFast(PIN_A, 1); digitalWriteFast(PIN_B, 1); digitalWriteFast(PIN_C, 1);

    /* ---- 2. bring it up ourselves -------------------------------------------------------- */
    Serial.println(F("\n[2] setting FlexSPI2 to 49.5 MHz and identifying the chip"));
    flexspi2_clock(0, 7);                 /* 396 MHz source, divided by 8 */
    const uint32_t id = read_id();
    Serial.print(F("    identity: 0x"));
    Serial.println(id, HEX);

    if ((id & 0xFFFF) != 0x5D0D && (id & 0xFFFF) != 0x5D9D) {
        Serial.println(F("    no chip answered even at 49.5 MHz. Stopping here rather than"));
        Serial.println(F("    mapping a window onto nothing."));
        return;
    }

    FLEXSPI2_FLSHA1CR0 = 8 << 10;         /* 8 MB on the first chip select */
    ip_cmd(4, 0);                          /* enter quad mode, which the AHB window needs */
    external_psram_size = 8;
    Serial.println(F("    mapped 8 MB at 0x70000000 and switched the chip to quad"));

    /* ---- 3. does the memory actually hold data through the window? ------------------------ */
    Serial.println(F("\n[3] writing and reading 64 kB through the memory window"));
    pick(PARK);
    fill(0x3B);
    const uint32_t bad = verify(0x3B);
    Serial.print(F("    wrong bytes: "));
    Serial.println(bad);

    if (bad) {
        Serial.println(F("    the window is mapped but the data does not survive. 49.5 MHz is"));
        Serial.println(F("    still too fast for this wiring, and the controller cannot go"));
        Serial.println(F("    slower. Shortening the stubs is the only way from here."));
        return;
    }
    Serial.println(F("    it holds. YOU HAVE 8 MB OF WORKING PSRAM."));

    /* ---- 4. settle the contention question with real data --------------------------------- *
     * Identical bytes prove nothing: two chips driving the same value do not conflict, which is
     * why the identity read looked clean on Y0. Give the two chips DIFFERENT contents and read
     * again -- now they have to disagree on the wire.                                          */
    Serial.println(F("\n[4] do the onboard chip and a banked chip collide?"));
    pick(0); fill(0xA1);                   /* onboard + banked chip 1 both receive A1 */
    pick(1); fill(0xB2);                   /* onboard + banked chip 2 both receive B2 */

    pick(0);
    const uint32_t bad0 = verify(0xA1);    /* onboard holds B2 now, banked chip 1 holds A1 */
    pick(1);
    const uint32_t bad1 = verify(0xB2);    /* onboard holds B2, banked chip 2 holds B2: agree */
    pick(PARK);

    Serial.print(F("    reading Y0, where the two chips now disagree: "));
    Serial.print(bad0); Serial.println(F(" wrong"));
    Serial.print(F("    reading Y1, where they happen to agree:       "));
    Serial.print(bad1); Serial.println(F(" wrong"));

    Serial.println(F("\n--- verdict ---"));
    if (bad0 > 0 && bad1 == 0) {
        Serial.println(F("  Collision confirmed. The onboard chip answers on every decoder"));
        Serial.println(F("  output, so the five banked chips can never be read on their own."));
        Serial.println(F("  You have 8 MB now, at 49.5 MHz, parked on Y7. To get the other 40:"));
        Serial.println(F("    - remove the onboard chip, or"));
        Serial.println(F("    - lift its pin 1 and run that leg to decoder pin 10 (Y5),"));
        Serial.println(F("      which turns it into a sixth bank instead of a rival."));
    } else if (bad0 == 0 && bad1 == 0) {
        Serial.println(F("  No collision. Both banks read back correctly, which means the"));
        Serial.println(F("  onboard chip is NOT answering alongside them after all."));
        Serial.println(F("  All six chips are usable. Nothing needs unsoldering."));
    } else {
        Serial.println(F("  Both reads are wrong, so this is not a collision between two chips."));
        Serial.println(F("  The memory is marginal at 49.5 MHz. Shorten the stubs."));
    }
    Serial.println(F("\n=== done ==="));
}

void loop() { }
