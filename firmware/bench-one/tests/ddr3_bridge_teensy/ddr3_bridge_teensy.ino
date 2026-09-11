/* ===========================================================================================
 *  ddr3_bridge_teensy -- put a DDR3 DIMM behind a Teensy 4.1's memory bus, through an FPGA
 * ===========================================================================================
 *
 *  The FPGA is a wire, not a worker. It does no arithmetic: it answers quad SPI transactions by
 *  moving bytes to and from DDR3, unchanged and in order. Everything measured here is therefore what
 *  a TEENSY can do when it has real RAM behind it.
 *
 *  WHAT THIS SKETCH IS FOR, AND WHY IT MATTERS MORE THAN THE BENCHMARK
 *  ------------------------------------------------------------------
 *  One number in this whole design cannot be calculated, only found: where to sample read data. With
 *  the DLL disabled the datasheet allows the strobe to come back more than a whole clock late and
 *  declines to bound it, so the controller samples at a fixed offset instead. Guessing that offset
 *  by rebuilding the bitstream costs a synthesis, a place and route and a reflash per attempt.
 *
 *  So the gateware takes it at RUNTIME and this sketch sweeps it. SECTION 3 below writes a pattern,
 *  then tries every latency and sample position, and prints a grid of which ones return the data
 *  intact. One flash, and the board tells you its own timing.
 *
 *  WHERE THE CEILING IS
 *  --------------------
 *  A Teensy 4.1's widest external bus is FlexSPI2 port A, four data lines, on the bottom-side pads:
 *
 *      pin 53  SCLK     pin 52  IO0     pin 49  IO1     pin 50  IO2     pin 54  IO3
 *      pin 48  SS0 = the onboard PSRAM, left alone
 *      pin 51  SS1 = free on any board without a second PSRAM. The FPGA goes here.
 *
 *  Per the RT1060 datasheet tables 38 and 42, with the read strobe looped back through the DQS pad,
 *  which is what the core configures, the bus allows 133 MHz single rate or 66 MHz double rate. Four
 *  lines at 133 MHz is 66.5 MB/s either way, so double rate buys nothing. The 166 MHz row needs the
 *  device to drive DQS and that pad, EMC_23, is not brought out on a 4.1. The parallel memory
 *  controller needs 41 pads and 16 are bonded; FlexSPI2's second port sits on pads EMC_13 to 16,
 *  which appear nowhere in the core. So 66.5 MB/s is the hard ceiling, against 33.9 measured for the
 *  onboard PSRAM: exactly 2x of headroom, and no more.
 *
 *  THE RATE RULE, WHICH DECIDES EVERY SETTING BELOW
 *  -----------------------------------------------
 *  FlexSPI cannot be stalled. Once the dummy cycles end it clocks data out relentlessly, so the
 *  memory must supply bytes at least as fast as the Teensy takes them, or the reads run ahead of the
 *  data and return whatever the buffer held before. Eight DDR3 data lines deliver 8 bytes every four
 *  memory clocks, and four FlexSPI lines carry half a byte per clock:
 *
 *      memory clock   DDR3 supplies   link must be under   FlexSPI setting
 *       6.25 MHz       12.5 MB/s       25 MHz               unreachable, see MEM_MHZ 6
 *      12.50 MHz       25.0 MB/s       50 MHz               49.5 MHz
 *      25.00 MHz       50.0 MB/s      100 MHz               88 MHz, for margin
 *
 *  "At least as fast" is not enough: it has to be FASTER, because every DRAM request carries some
 *  fixed overhead no matter how the chunks are sized. Pairing a 99 MHz link with a 25 MHz memory is
 *  49.5 MB/s against 50 and the end-to-end simulation fails; 88 MHz is 44 against 50 and it passes.
 *
 *  The slowest FlexSPI2 can run is 49.5 MHz, its slowest source divided by eight. That is why a
 *  6.25 MHz memory clock needs short transactions instead: the link cannot be slowed to match it, so
 *  each transaction is made small enough that the dummy window covers the whole fetch.
 *
 *  EXPECTED RESULTS, from the cost model that already predicts the PSRAM number
 *  --------------------------------------------------------------------------
 *  Reading and unpacking ADD on a CPU; only fabric overlaps them. With the measured 39.3 MB/s unpack
 *  rate, effective throughput is 1/(1/read + 1/unpack):
 *
 *      PSRAM            33.9 raw -> 18.2 effective    (this matches the measured 18.2, which is the
 *      12.5 MHz memory  25.0 raw -> 15.3 effective     reason the model is trusted for the rest)
 *      25 MHz memory    50.0 raw -> 22.0 effective
 *      link-limited     66.5 raw -> 24.7 effective
 *      infinite link             -> 39.3 effective
 *
 *  So first light is SLOWER than the PSRAM, and that is fine: it is proving 256 MB works at all.
 *  Speed arrives with the faster translators. And note where it stops: past about 66 MB/s the
 *  Teensy's own nibble unpacking is the wall and more bus speed buys nothing, so the next real win
 *  after that is in the unpack kernel, not the memory.
 *
 *  CAPACITY
 *  --------
 *  Eight data lines reach ONE chip. A DIMM rank is eight x8 devices sharing address and command,
 *  each supplying eight of sixty-four data bits, so eight wires see one device: 8 banks x 32768 rows
 *  x 1 KB = 256 MB. The rest of a 4 GB stick is behind the other fifty-six lines. 256 MB is sixteen
 *  times the whole PSRAM bank and very nearly fills the 240 MB window the Teensy can map.
 *
 *  FLASH THIS WITH NO DIMM WIRED FIRST. It will report that the FPGA did not answer, which confirms
 *  the sketch runs and the bus is alive before any 1.5 V part is at risk.
 * ======================================================================================== */

#include <Arduino.h>

/* ============================================================================================
 *  SECTION 1 -- configuration. MEM_MHZ must match what the bitstream was built for.
 * ========================================================================================= */

/* The gateware's CK_DIV as a memory clock in MHz: 6 means CK_DIV 16, 12 means 8, 25 means 4. */
#define MEM_MHZ          6

#if   MEM_MHZ == 25
  /* 79.2 MHz is 39.6 MB/s against 50 MB/s of memory, a 21% margin, and it is the fastest setting
   * verified end to end. 88 and 99 MHz were both tried: 99 is 49.5 against 50 and fails outright,
   * and 88 is 44 against 50, which is closer to the edge than anything simulated. */
  #define FLEXSPI_MHZ    79
  #define FPGA_DUMMY     200
  #define AHB_BUFSZ      32     /* 256-byte transactions; must stay under the gateware's RD_AHEAD */

#elif MEM_MHZ == 12
  /* THIS CONFIGURATION CANNOT WORK, and the compiler stops here rather than letting it be wired.
   *
   * A 12.5 MHz memory clock delivers 25 MB/s on eight data lines. The slowest FlexSPI2 can be made to
   * run is 49.5 MHz, its slowest source divided by eight, which consumes 24.75 MB/s. That is a 1%
   * margin, and 1% is not a margin: every DRAM request carries fixed overhead, so the reader catches
   * up and returns bytes that have not arrived. The end-to-end simulation fails three of its seven
   * cases at exactly this pairing, and passes at 41.7 MB/s -- a link speed FlexSPI2 cannot reach.
   *
   * Use MEM_MHZ 25. If the level translators cannot carry 50 Mb/s data lines, the fix is a wider DDR3
   * bus, not a slower one: see the note on MEM_MHZ 6. */
  #error "MEM_MHZ 12 is unreachable: the FlexSPI2 clock floor of 49.5 MHz outruns a 12.5 MHz memory"

#elif MEM_MHZ == 6
  /* THE SLOW PATH, AND THE ONLY ONE THAT WORKS WITH WEAK LEVEL TRANSLATORS.
   *
   * Four data lines cannot be used here. FlexSPI2's slowest clock is 49.5 MHz, which on four lines
   * consumes 24.75 MB/s, and a 6.25 MHz memory delivers 12.5 -- the link outruns it two to one and no
   * dummy count fixes a sustained rate. ONE line at the same clock consumes 6.2 MB/s, which leaves a
   * factor of two in hand.
   *
   * The payoff is the data lines: 6.25 MHz means 12.5 Mb/s, inside a TXB0108's rating, where 25 MHz
   * means 50 Mb/s and is not. So this configuration is slow -- about a fifth of the onboard PSRAM --
   * and it proves 256 MB of DDR3 works using only translators most people already own.
   *
   * SINGLE_BIT switches the read LUT to one pin. Writes stay in quad mode: they are rare, they are not
   * rate-critical, and the write path has the whole transaction to drain. */
  #define FLEXSPI_MHZ    50
  #define FPGA_DUMMY     200
  #define AHB_BUFSZ      32
  #define SINGLE_BIT     1

#else
  #error "MEM_MHZ must be 25, and must match the gateware's CK_DIV of 4"
#endif

/* A belt-and-braces check on the rule above, in case someone edits the numbers rather than the mode.
 * Memory delivers 8 bytes per 4 memory clocks, so 2 * MEM_MHZ megabytes per second. The link carries
 * half a byte per clock, so FLEXSPI_MHZ / 2. Demand at least 15% of headroom. */
#ifndef SINGLE_BIT
  #define SINGLE_BIT 0
#endif

/* A belt-and-braces check on the rate rule, in case someone edits the numbers rather than the mode.
 * Memory delivers 8 bytes per 4 memory clocks, so 2 * MEM_MHZ MB/s. A four-line link carries half a
 * byte per clock; a one-line link carries an eighth. Demand 15% of headroom either way. */
#if SINGLE_BIT
  #if (FLEXSPI_MHZ * 100) > (2 * MEM_MHZ * 8 * 85)
    #error "single-bit link too fast for the memory: FLEXSPI_MHZ/8 must be under 85% of 2*MEM_MHZ"
  #endif
#else
#if (FLEXSPI_MHZ * 100) > (2 * MEM_MHZ * 2 * 85)
  #error "the link is too fast for the memory: FLEXSPI_MHZ/2 must be under 85% of 2*MEM_MHZ"
#endif
#endif

#define FPGA_MB          224    /* 240 MB aperture minus the PSRAM's 16 */

/* Sweep bounds for the read calibration. The nominal latency is CAS 6 plus one for the controller's
 * falling-edge command register; late strobes need more. */
#define SWEEP_LAT_LO     4
#define SWEEP_LAT_HI     11
#define SWEEP_SAMP_MAX   (MEM_MHZ == 25 ? 4 : (MEM_MHZ == 12 ? 8 : 16))

/* LUT sequence slots. 0 to 6 belong to the core's PSRAM setup; these are ours. */
#define SEQ_FPGA_RD      8
#define SEQ_FPGA_WR      9
#define SEQ_FPGA_CFG     10
#define SEQ_FPGA_WIN     11

#define LUT0(op, pads, opnd) (FLEXSPI_LUT_INSTRUCTION((op), (pads), (opnd)))
#define LUT1(op, pads, opnd) (FLEXSPI_LUT_INSTRUCTION((op), (pads), (opnd)) << 16)
#define CMD_SDR    FLEXSPI_LUT_OPCODE_CMD_SDR
#define ADDR_SDR   FLEXSPI_LUT_OPCODE_RADDR_SDR
#define READ_SDR   FLEXSPI_LUT_OPCODE_READ_SDR
#define WRITE_SDR  FLEXSPI_LUT_OPCODE_WRITE_SDR
#define DUMMY_SDR  FLEXSPI_LUT_OPCODE_DUMMY_SDR
#define PINS1      FLEXSPI_LUT_NUM_PADS_1
#define PINS4      FLEXSPI_LUT_NUM_PADS_4

extern "C" uint8_t external_psram_size;

static volatile uint32_t *const lut = &FLEXSPI2_LUT0;
static uint8_t  *fpga_base;
static uint32_t  fpga_bytes;
static uint32_t  a2_offset;      /* device-relative base, which is what the FPGA sees */

/* ============================================================================================
 *  SECTION 2 -- bus plumbing
 * ========================================================================================= */

static void ip_cmd(uint32_t seq, uint32_t addr)
{
    FLEXSPI2_IPCR0 = addr;
    FLEXSPI2_IPCR1 = FLEXSPI_IPCR1_ISEQID(seq);
    FLEXSPI2_IPCMD = FLEXSPI_IPCMD_TRG;
    while (!(FLEXSPI2_INTR & FLEXSPI_INTR_IPCMDDONE)) { }
    FLEXSPI2_INTR = FLEXSPI_INTR_IPCMDDONE;
}

/* Send a short payload with an IP command. Used for the two custom registers. */
static void ip_write(uint32_t seq, uint32_t addr, const uint8_t *data, uint32_t n)
{
    FLEXSPI2_IPTXFCR = FLEXSPI_IPTXFCR_CLRIPTXF;
    uint32_t w[2] = {0, 0};
    for (uint32_t i = 0; i < n && i < 8; i++) w[i >> 2] |= ((uint32_t)data[i]) << (8 * (i & 3));
    FLEXSPI2_TFDR0 = w[0];
    FLEXSPI2_TFDR1 = w[1];
    FLEXSPI2_IPCR0 = addr;
    FLEXSPI2_IPCR1 = FLEXSPI_IPCR1_ISEQID(seq) | FLEXSPI_IPCR1_IDATSZ(n);
    FLEXSPI2_IPCMD = FLEXSPI_IPCMD_TRG;
    /* The transmit watermark has to be acknowledged or the command never completes. */
    while (!(FLEXSPI2_INTR & FLEXSPI_INTR_IPCMDDONE)) {
        if (FLEXSPI2_INTR & FLEXSPI_INTR_IPTXWE) FLEXSPI2_INTR = FLEXSPI_INTR_IPTXWE;
    }
    FLEXSPI2_INTR = FLEXSPI_INTR_IPCMDDONE;
}

static uint32_t ip_read_id(uint32_t addr)
{
    FLEXSPI2_IPRXFCR = FLEXSPI_IPRXFCR_CLRIPRXF;
    FLEXSPI2_IPCR0 = addr;
    FLEXSPI2_IPCR1 = FLEXSPI_IPCR1_ISEQID(3) | FLEXSPI_IPCR1_IDATSZ(4);
    FLEXSPI2_IPCMD = FLEXSPI_IPCMD_TRG;
    while (!(FLEXSPI2_INTR & FLEXSPI_INTR_IPCMDDONE)) { }
    uint32_t id = FLEXSPI2_RFDR0;
    FLEXSPI2_INTR = FLEXSPI_INTR_IPCMDDONE | FLEXSPI_INTR_IPRXWA;
    return id;
}

/* FlexSPI2 root clock. The source and divider pairs come from the table the Teensy core carries in
 * configure_external_ram(); the core ships 105.6 MHz. Both devices share this clock, and the PSRAM
 * is rated to 133 MHz, so every entry here is safe for it. 49.5 MHz is the slowest the peripheral
 * can be made to run: the slowest source it can select, divided by eight. */
static bool flexspi2_set_clock(int mhz)
{
    uint32_t podf, sel;
    switch (mhz) {
        case 50:  podf = 7; sel = 0; break;   /* 396/8  = 49.5, the floor */
        case 57:  podf = 6; sel = 0; break;   /* 396/7  = 56.6 */
        case 66:  podf = 5; sel = 0; break;   /* 396/6  = 66.0 */
        case 79:  podf = 4; sel = 0; break;   /* 396/5  = 79.2 */
        case 88:  podf = 5; sel = 3; break;
        case 99:  podf = 3; sel = 0; break;
        case 106: podf = 4; sel = 3; break;
        case 120: podf = 5; sel = 1; break;
        case 132: podf = 3; sel = 3; break;
        default: return false;
    }
    FLEXSPI2_MCR0 |= FLEXSPI_MCR0_MDIS;
    CCM_CCGR7 &= ~CCM_CCGR7_FLEXSPI2(CCM_CCGR_ON);
    CCM_CBCMR = (CCM_CBCMR & ~(CCM_CBCMR_FLEXSPI2_PODF_MASK | CCM_CBCMR_FLEXSPI2_CLK_SEL_MASK))
              | CCM_CBCMR_FLEXSPI2_PODF(podf) | CCM_CBCMR_FLEXSPI2_CLK_SEL(sel);
    CCM_CCGR7 |= CCM_CCGR7_FLEXSPI2(CCM_CCGR_ON);
    FLEXSPI2_MCR0 &= ~FLEXSPI_MCR0_MDIS;
    FLEXSPI2_MCR0 |= FLEXSPI_MCR0_SWRESET;
    while (FLEXSPI2_MCR0 & FLEXSPI_MCR0_SWRESET) { }
    return true;
}

/* Tell the gateware where to sample read data. Four nibbles, high nibble of each byte first:
 * latency high, latency low, sample offset, then the prefetch bit. */
static void fpga_set_timing(uint8_t latency, uint8_t sample, bool prefetch)
{
    uint8_t b[2];
    b[0] = latency;
    b[1] = (uint8_t)((sample & 0x0F) << 4) | (prefetch ? 1 : 0);
    ip_write(SEQ_FPGA_CFG, a2_offset, b, 2);
    arm_dcache_flush_delete(fpga_base, 4096);   /* nothing cached may survive a timing change */
}

static bool fpga_init()
{
    uint32_t a1_mb = external_psram_size;
    uint32_t a2_mb = FPGA_MB;
    if (a1_mb + a2_mb > 240) a2_mb = 240 - a1_mb;   /* the aperture is 240 MB, RM page 35 */

    a2_offset  = a1_mb << 20;
    fpga_base  = (uint8_t *)(0x70000000u + a2_offset);
    fpga_bytes = a2_mb << 20;

    FLEXSPI2_LUTKEY = FLEXSPI_LUTKEY_VALUE;
    FLEXSPI2_LUTCR  = FLEXSPI_LUTCR_UNLOCK;

    /* Quad read. A 32-bit address because 24 bits reaches only 16 MB, and a long dummy window
     * because that window is the only place a DRAM access can hide: FlexSPI cannot be stalled. */
#if SINGLE_BIT
    /* One pin: command 0x03, a 32-bit address, the dummy window, then data on IO1. The gateware
     * decodes this only while NOT in quad mode, which is why the sketch never sends 0x35 below. */
    lut[4*SEQ_FPGA_RD + 0] = LUT0(CMD_SDR,   PINS1, 0x03) | LUT1(ADDR_SDR, PINS1, 32);
    lut[4*SEQ_FPGA_RD + 1] = LUT0(DUMMY_SDR, PINS1, FPGA_DUMMY) | LUT1(READ_SDR, PINS1, 1);
#else
    lut[4*SEQ_FPGA_RD + 0] = LUT0(CMD_SDR,   PINS4, 0xEB) | LUT1(ADDR_SDR, PINS4, 32);
    lut[4*SEQ_FPGA_RD + 1] = LUT0(DUMMY_SDR, PINS4, FPGA_DUMMY) | LUT1(READ_SDR, PINS4, 1);
#endif
    lut[4*SEQ_FPGA_RD + 2] = 0;
    lut[4*SEQ_FPGA_RD + 3] = 0;

#if SINGLE_BIT
    /* Writes stay single-bit too, so no mode switching is needed mid-stream. They are rare and not
     * rate-critical: the whole transaction is available to drain them. */
    lut[4*SEQ_FPGA_WR + 0] = LUT0(CMD_SDR, PINS1, 0x38) | LUT1(ADDR_SDR, PINS1, 32);
    lut[4*SEQ_FPGA_WR + 1] = LUT0(WRITE_SDR, PINS1, 1);
#else
    lut[4*SEQ_FPGA_WR + 0] = LUT0(CMD_SDR, PINS4, 0x38) | LUT1(ADDR_SDR, PINS4, 32);
    lut[4*SEQ_FPGA_WR + 1] = LUT0(WRITE_SDR, PINS4, 1);
#endif
    lut[4*SEQ_FPGA_WR + 2] = 0;
    lut[4*SEQ_FPGA_WR + 3] = 0;

    lut[4*SEQ_FPGA_CFG + 0] = LUT0(CMD_SDR, PINS4, 0xC1) | LUT1(WRITE_SDR, PINS4, 1);
    lut[4*SEQ_FPGA_CFG + 1] = 0;

    lut[4*SEQ_FPGA_WIN + 0] = LUT0(CMD_SDR, PINS4, 0xC0) | LUT1(WRITE_SDR, PINS4, 1);
    lut[4*SEQ_FPGA_WIN + 1] = 0;

    /* Point the A2 device at our sequences. A1 keeps the core's, so the PSRAM is untouched. */
    FLEXSPI2_FLSHA2CR0 = a2_mb << 10;
    FLEXSPI2_FLSHA2CR2 = FLEXSPI_FLSHCR2_AWRSEQID(SEQ_FPGA_WR) | FLEXSPI_FLSHCR2_AWRSEQNUM(0)
                       | FLEXSPI_FLSHCR2_ARDSEQID(SEQ_FPGA_RD) | FLEXSPI_FLSHCR2_ARDSEQNUM(0);

    /* Transaction length. At slow memory clocks this must be small enough that the dummy window
     * covers the entire fetch, because nothing downstream can ask the Teensy to wait. */
    uint32_t mask = (FLEXSPI_AHBRXBUFCR0_PREFETCHEN | FLEXSPI_AHBRXBUFCR0_PRIORITY_MASK
                   | FLEXSPI_AHBRXBUFCR0_MSTRID_MASK | FLEXSPI_AHBRXBUFCR0_BUFSZ_MASK);
    FLEXSPI2_AHBRXBUF1CR0 = (FLEXSPI2_AHBRXBUF1CR0 & ~mask)
                          | FLEXSPI_AHBRXBUFCR0_PREFETCHEN | FLEXSPI_AHBRXBUFCR0_BUFSZ(AHB_BUFSZ);

    FLEXSPI2_MCR0 |= FLEXSPI_MCR0_SWRESET;
    while (FLEXSPI2_MCR0 & FLEXSPI_MCR0_SWRESET) { }

    /* The gateware comes up in single-bit mode, as a real part does, so a stock Teensy probe finds
     * it. Sequence 4 is the core's "enter quad mode", 0x35. */
    uint32_t id = ip_read_id(a2_offset);
    Serial.printf("    identity on chip select 1: 0x%08lX\n", (unsigned long)id);
    if ((id & 0xFFFF) != 0x5D9D) return false;

    /* The gateware reports the CK_DIV it was built with in the top byte of its identity. Checking it
     * here turns a mismatch from a silent wrong-data fault into one printed line. The dummy-cycle
     * count and the maximum transaction length both depend on it, so disagreeing is not survivable:
     * the reads would be fast, confident and wrong, which reads exactly like a wiring fault. */
    uint32_t ck_div   = (id >> 24) & 0xFF;
    uint32_t want_div = (MEM_MHZ == 25) ? 4 : (MEM_MHZ == 12 ? 8 : 16);
    if (ck_div != want_div) {
        Serial.printf("    MISMATCH: bitstream built for CK_DIV %lu, a %lu MHz memory clock,\n",
                      (unsigned long)ck_div, (unsigned long)(100 / (ck_div ? ck_div : 1)));
        Serial.printf("    but MEM_MHZ is %d here, which expects CK_DIV %lu.\n",
                      MEM_MHZ, (unsigned long)want_div);
        Serial.println("    Set MEM_MHZ at the top of this sketch to match, and reflash.");
        Serial.println("    Continuing would produce fast, confident, wrong data.");
        return false;
    }
    Serial.printf("    gateware confirms CK_DIV %lu, a %lu MHz memory clock\n",
                  (unsigned long)ck_div, (unsigned long)(100 / ck_div));

#if SINGLE_BIT
    /* Deliberately NOT entering quad mode: the single-bit read command is only decoded outside it.
     * Writes below enter quad mode briefly and leave it again. */
    Serial.println("    single-bit read path: slow, and it works with weak level translators");
#else
    ip_cmd(4, a2_offset);
#endif
    return true;
}

/* ============================================================================================
 *  SECTION 3 -- the calibration sweep, which is the point of this sketch
 * ========================================================================================= */

#define CAL_BYTES 256
static uint8_t cal_ref[CAL_BYTES];

static uint32_t cal_compare()
{
    volatile uint8_t *p = (volatile uint8_t *)fpga_base;
    arm_dcache_flush_delete((void *)p, CAL_BYTES);
    uint32_t bad = 0;
    for (uint32_t i = 0; i < CAL_BYTES; i++) if (p[i] != cal_ref[i]) bad++;
    return bad;
}

/* Writes do not depend on read timing, so the pattern can be laid down once and then read back at
 * every candidate setting. If every setting fails, the fault is in the write path or the wiring,
 * not the calibration, and the sweep says so rather than leaving it ambiguous. */
static bool calibrate(uint8_t *best_lat, uint8_t *best_samp)
{
    volatile uint8_t *p = (volatile uint8_t *)fpga_base;
    for (uint32_t i = 0; i < CAL_BYTES; i++) cal_ref[i] = (uint8_t)(i * 0x9D + 0x3B);

    /* Lay the pattern down at the nominal setting. */
    fpga_set_timing(MEM_MHZ == 25 ? 6 : 6, 1, true);
    for (uint32_t i = 0; i < CAL_BYTES; i++) p[i] = cal_ref[i];
    arm_dcache_flush_delete((void *)p, CAL_BYTES);

    Serial.println();
    Serial.println("    read calibration: rows are latency in memory clocks, columns are");
    Serial.println("    the sample offset within one. A dot is a clean 256-byte read back.");
    Serial.print("\n          sample:");
    for (int s = 0; s < SWEEP_SAMP_MAX; s++) Serial.printf("%3d", s);
    Serial.println();

    int best_run = 0, run = 0;
    uint8_t run_lat = 0, run_start = 0;
    *best_lat = 0; *best_samp = 0;

    for (int l = SWEEP_LAT_LO; l <= SWEEP_LAT_HI; l++) {
        Serial.printf("    latency %2d:   ", l);
        run = 0;
        for (int s = 0; s < SWEEP_SAMP_MAX; s++) {
            fpga_set_timing((uint8_t)l, (uint8_t)s, true);
            uint32_t bad = cal_compare();
            Serial.print(bad == 0 ? "  ." : (bad < 16 ? "  ~" : "  X"));
            if (bad == 0) {
                if (run == 0) { run_lat = (uint8_t)l; run_start = (uint8_t)s; }
                run++;
                /* Keep the MIDDLE of the longest clean run, not its first entry: the centre of a
                 * window has margin on both sides, and margin is the whole point of calibrating. */
                if (run > best_run) {
                    best_run   = run;
                    *best_lat  = run_lat;
                    *best_samp = (uint8_t)(run_start + (run - 1) / 2);
                }
            } else {
                run = 0;
            }
        }
        Serial.println();
    }

    Serial.println();
    if (best_run == 0) {
        Serial.println("    NO setting worked. This is not a calibration problem:");
        Serial.println("      - if the identity read succeeded, the QSPI link is fine and the fault");
        Serial.println("        is on the DDR3 side: check VREF at contacts 1 and 67 first, then");
        Serial.println("        the tie-offs, then that the initialisation indicator is lit.");
        Serial.println("      - if writes are landing but reads never match at any offset, suspect");
        Serial.println("        the data lines are translated in only one direction.");
        return false;
    }
    Serial.printf("    widest clean window is %d wide; using latency %d, sample %d\n",
                  best_run, *best_lat, *best_samp);
    if (best_run == 1)
        Serial.println("    WARNING: only one setting worked, so there is no timing margin at all.");
    fpga_set_timing(*best_lat, *best_samp, true);
    return true;
}

/* ============================================================================================
 *  SECTION 4 -- tests and measurement
 * ========================================================================================= */

static uint32_t cyc() { return ARM_DWT_CYCCNT; }
static float    mbs(uint32_t bytes, uint32_t cycles)
{
    return (float)bytes * (F_CPU_ACTUAL / 1.0e6f) / (float)cycles;
}

/* A walking value catches stuck address lines, which a constant fill never does. */
static bool mem_test(volatile uint32_t *p, uint32_t words, const char *what)
{
    Serial.printf("    %s (%lu KB)...", what, (unsigned long)(words * 4 / 1024));
    for (uint32_t i = 0; i < words; i++) p[i] = i * 2654435761u;
    arm_dcache_flush_delete((void *)p, words * 4);

    uint32_t bad = 0, first = 0xFFFFFFFF;
    for (uint32_t i = 0; i < words; i++) {
        if (p[i] != i * 2654435761u) { if (!bad) first = i; bad++; }
    }
    if (bad) {
        Serial.printf(" FAILED: %lu of %lu words, first at word %lu\n",
                      (unsigned long)bad, (unsigned long)words, (unsigned long)first);
        Serial.printf("      wrote 0x%08lX, read 0x%08lX\n",
                      (unsigned long)(first * 2654435761u), (unsigned long)p[first]);
        return false;
    }
    Serial.println(" all words match");
    return true;
}

static float read_rate(volatile uint32_t *p, uint32_t bytes)
{
    arm_dcache_flush_delete((void *)p, bytes);
    uint32_t n = bytes / 4, acc = 0, t0 = cyc();
    for (uint32_t i = 0; i < n; i++) acc += p[i];
    uint32_t dt = cyc() - t0;
    asm volatile("" :: "r"(acc));
    return mbs(bytes, dt);
}

/* Read plus unpack, which is what actually predicts tokens per second. Q4_K packs 256 weights into
 * 144 bytes; this is the nibble half, which dominates. */
static float unpack_rate(volatile uint8_t *p, uint32_t bytes)
{
    arm_dcache_flush_delete((void *)p, bytes);
    int32_t acc = 0;
    uint32_t t0 = cyc();
    for (uint32_t i = 0; i < bytes; i++) {
        uint8_t b = p[i];
        acc += (int32_t)(b & 0x0F) - 8;
        acc += (int32_t)(b >> 4)   - 8;
    }
    uint32_t dt = cyc() - t0;
    asm volatile("" :: "r"(acc));
    return mbs(bytes, dt);
}

void setup()
{
    Serial.begin(115200);
    while (!Serial && millis() < 4000) { }
    ARM_DEMCR    |= ARM_DEMCR_TRCENA;
    ARM_DWT_CTRL |= ARM_DWT_CTRL_CYCCNTENA;

    Serial.println();
    Serial.println("=== DDR3 behind a Teensy 4.1, through an FPGA acting as a wire ===");
    Serial.printf("  CPU %lu MHz, onboard PSRAM %u MB\n",
                  (unsigned long)(F_CPU_ACTUAL / 1000000), external_psram_size);
    Serial.printf("  built for a %d MHz memory clock: link %d MHz, %d dummy cycles, %d-byte reads\n",
                  MEM_MHZ, FLEXSPI_MHZ, FPGA_DUMMY, AHB_BUFSZ * 8);

    if (!flexspi2_set_clock(FLEXSPI_MHZ)) {
        Serial.printf("  %d MHz is not in the clock table; stopping\n", FLEXSPI_MHZ);
        return;
    }

    if (external_psram_size > 0) {
        volatile uint32_t *ps = (volatile uint32_t *)0x70000000u;
        uint32_t n = 4u << 20;
        Serial.println("  --- onboard PSRAM, the control ---");
        Serial.printf("    sequential read %.1f MB/s, read+unpack %.1f MB/s\n",
                      read_rate(ps, n), unpack_rate((volatile uint8_t *)ps, n));
    }

    Serial.println("  --- the FPGA bridge ---");
    if (!fpga_init()) {
        Serial.println("    no answer, and nothing is at risk. Check SCLK on pin 53, chip select");
        Serial.println("    on pin 51, IO0 pin 52, IO1 pin 49, IO2 pin 50, IO3 pin 54, and that the");
        Serial.println("    two boards share a ground. The FPGA must be configured first.");
        return;
    }
    Serial.printf("    mapped %lu MB at 0x%08lX\n",
                  (unsigned long)(fpga_bytes >> 20), (unsigned long)fpga_base);

    uint8_t lat, samp;
    if (!calibrate(&lat, &samp)) return;

    if (!mem_test((volatile uint32_t *)fpga_base, 16u * 1024 / 4, "64 KB")) return;
    if (!mem_test((volatile uint32_t *)fpga_base, 8u << 18, "8 MB")) return;

    Serial.println("    holding 5 s, so only refresh can be keeping the data alive...");
    delay(5000);
    if (!mem_test((volatile uint32_t *)fpga_base, 16u * 1024 / 4, "after the hold")) {
        Serial.println("    data decayed: refresh is not working.");
        return;
    }

    uint32_t n = 8u << 20;
    float r = read_rate((volatile uint32_t *)fpga_base, n);
    float u = unpack_rate((volatile uint8_t *)fpga_base, n);
    float link = FLEXSPI_MHZ / 2.0f;
    float mem  = (MEM_MHZ == 25) ? 50.0f : (MEM_MHZ == 12 ? 25.0f : 12.5f);
    Serial.println();
    Serial.printf("    sequential read  %.1f MB/s\n", r);
    Serial.printf("    read and unpack  %.1f MB/s\n", u);
    Serial.printf("    link ceiling %.1f, memory ceiling %.1f, so the limit here is the %s\n",
                  link, mem, (mem < link) ? "memory" : "link");
    Serial.printf("    using latency %d, sample %d\n", lat, samp);
}

void loop() { }
