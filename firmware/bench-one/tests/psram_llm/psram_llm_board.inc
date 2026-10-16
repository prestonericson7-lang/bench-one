
/* ===========================================================================================
 *  EVERYTHING BELOW IS NEW IN THIS SKETCH. EVERYTHING ABOVE IS psram_worker's BUS LAYER, VERBATIM.
 * ======================================================================================== */

/* ---------------------------------------------------------------------------------------------
 *  QUALIFICATION -- luckfox/drive.py's procedure, run by the Teensy on itself
 *
 *  Same order and the same numbers: discover by identity, then quad first and the two single-bit reads
 *  after it, each at chip-select setup 0 and then 16. The fastest read that verifies against a slow
 *  write, then the fastest write against that read, a burst computed from the 21 us chip-select budget
 *  in BOTH directions, and a confirmation that reads the same span back four times from one fill,
 *  stepping back until it does. Every bank switch resets the chip, as drive.py does -- the benchmark
 *  found that reset load-bearing (benchmark-must-check-its-answer).
 *
 *  It runs HERE rather than being copied from an old log because a no-op count is a timing
 *  specification for one exact binary, and this is a different binary from the one those logs measured.
 * ------------------------------------------------------------------------------------------ */
#define USABLE            19
#define SAFE_BURST        16
#define WHOLD             15
#define PROBE             32768u
#define SPAN              (1024u * 1024u)
#define CONFIRM_PASSES    4
#define STABLE_PASSES     6
#define WAITS_QUAD        6
#define WAITS_0B          8
#define PAD_MODE          2
#define CS_LOW_BUDGET_US  21.0

/* THE WINDOW, NOT THE EDGE (measure-the-window-not-the-edge, docs/41).
 *
 * The sweep returns the FASTEST setting that verifies, which is the edge of a cliff. The first run of this
 * sketch proved it again: CS0 qualified at write 0 read 13, passed 1 MB four times, six stability passes
 * and the full bank-tagged 8 MB -- and then returned a wrong offset-table word for tokenizer piece 87,272,
 * whose data (small integers, mostly zero bytes) is not the test pattern. So every bank runs this many
 * table steps SLOWER than its edge, in both directions, and is confirmed there. PSRAM carries about 1 MB
 * a token against 1.9 GB off the card, so the speed this costs does not show. */
#define MARGIN            3

/* bank_t lives in psram_llm.h so Arduino's generated prototypes can see it */
static bank_t cand[10];
static bank_t use[10];                    /* the virtual PSRAM, fastest first */
static int    nuse;
static int    cur_bank = -1;

static const char *const MODE_NAME[3] = { "quad", "single 0x03", "single 0x0B" };

static void say(const char *fmt, ...)
{
    char b[240];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    Serial.println(b);
}

/* drive.py's select(): every knob restated, then the bank reset into its mode, then its timing. */
static void apply(const bank_t &b)
{
    g_rmode = b.mode;
    g_dummy = WAITS_QUAD;
    set_pads(PAD_MODE);
    if (b.mode == 2) g_dummy1 = WAITS_0B;
    g_csu = b.su;
    g_gap = 0;
    select_route(b.kind, b.y);
    enter_quad_here();
    set_pads(PAD_MODE);
    g_wi = b.wi; g_ri = b.ri; g_burst = b.nb;
}

static void fill(uint32_t a, uint32_t len)
{
    for (uint32_t off = 0; off < len; off += BLK) {
        const uint32_t n = (len - off > BLK) ? BLK : (len - off);
        for (uint32_t i = 0; i < n; i++) buf[i] = pat(a + off + i);
        do_write(a + off, buf, n);
    }
}

static uint32_t verify(uint32_t a, uint32_t len)
{
    uint32_t bad = 0;
    for (uint32_t off = 0; off < len; off += BLK) {
        const uint32_t n = (len - off > BLK) ? BLK : (len - off);
        do_read(a + off, buf, n);
        for (uint32_t i = 0; i < n; i++) if (buf[i] != pat(a + off + i)) bad++;
    }
    return bad;
}

/* drive.py burst_bound(): two timed bursts give the fixed cost and the per-byte cost. */
static int burst_bound(char dir)
{
    uint32_t c[2];
    const uint32_t len[2] = { 96, 32 };
    for (int k = 0; k < 2; k++) {
        const uint32_t save = g_burst;
        g_burst = len[k];
        if (dir == 'W') for (uint32_t i = 0; i < len[k]; i++) buf[i] = pat(i);
        const uint32_t t0 = ARM_DWT_CYCCNT;
        if (dir == 'W') do_write(0, buf, len[k]); else do_read(0, buf, len[k]);
        c[k] = ARM_DWT_CYCCNT - t0;
        g_burst = save;
    }
    const double per = ((double)c[0] - (double)c[1]) / 64.0;
    const double fix = (double)c[0] - per * 96.0;
    if (per <= 0) return SAFE_BURST;
    int n = (int)((CS_LOW_BUDGET_US * 1e-6 * (double)F_CPU_ACTUAL - fix) / per);
    n = (n / 8) * 8;
    if (n > 96) n = 96;
    if (n < 8) n = 8;
    return n;
}
static int pick_burst(void)
{
    const int r = burst_bound('R'), w = burst_bound('W');
    return r < w ? r : w;
}

/* THE EDGE UNDER THE MODEL'S KIND OF DATA. The edge was found with pat() alone, a linear byte sequence (each
 * byte the last + 0x9D). 2026-09-27: Y1 in 0x03 mode measured its read edge at 11 on the 12:27 boot and at 6 on
 * the 16:28 boot; every other bank's edges repeated exactly. Run at 6 + MARGIN = 9, it passed confirm, stable and
 * the 8 MB proof, then failed the attention cache's writes at the same two addresses on two prompts in a row
 * (17,131,792 read, 17,656,080 write). One lucky 32 KB probe had put it five steps past its real edge. So a
 * setting is clean only when four kinds of data all read back: the linear pattern, mostly-zero bytes (offset
 * tables), mostly-0xFF bytes, and pseudo-random bytes (float cache rows look like this). */
#define PROBE_KINDS 4
static inline uint8_t probe_byte(int kind, uint32_t i)
{
    switch (kind) {
    case 0:  return pat(i);
    case 1:  return (i % 61u) ? 0x00 : (uint8_t)(1u << (i % 8u));
    case 2:  return (i % 67u) ? 0xFF : (uint8_t)~(1u << (i % 8u));
    default: { uint32_t h = (i + 0x9E3779B9u) * 2654435761u; h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12;
               return (uint8_t)(h >> 8); }
    }
}
static bool probe_clean(void)
{
    for (int kind = 0; kind < PROBE_KINDS; kind++) {
        for (uint32_t off = 0; off < PROBE; off += BLK) {
            const uint32_t n = (PROBE - off > BLK) ? BLK : (PROBE - off);
            for (uint32_t i = 0; i < n; i++) buf[i] = probe_byte(kind, off + i);
            do_write(off, buf, n);
        }
        for (uint32_t off = 0; off < PROBE; off += BLK) {
            const uint32_t n = (PROBE - off > BLK) ? BLK : (PROBE - off);
            do_read(off, buf, n);
            for (uint32_t i = 0; i < n; i++) if (buf[i] != probe_byte(kind, off + i)) return false;
        }
    }
    return true;
}

static bool sweep(bank_t &b)
{
    b.wi = WHOLD; b.ri = USABLE - 1; b.nb = SAFE_BURST;
    apply(b);
    int ri_ok = -1;
    for (int ri = 0; ri < USABLE; ri++) {
        g_wi = WHOLD; g_ri = (uint8_t)ri; g_burst = SAFE_BURST;
        if (probe_clean()) { ri_ok = ri; break; }
    }
    if (ri_ok < 0) return false;
    int wi_ok = WHOLD;
    for (int wi = 0; wi < USABLE; wi++) {
        g_wi = (uint8_t)wi; g_ri = (uint8_t)ri_ok; g_burst = SAFE_BURST;
        if (probe_clean()) { wi_ok = wi; break; }
    }
    /* the edge, remembered for the log, then MARGIN steps back from it in both directions */
    b.ewi = (uint8_t)wi_ok; b.eri = (uint8_t)ri_ok;
    b.wi = (uint8_t)((wi_ok + MARGIN < USABLE) ? wi_ok + MARGIN : USABLE - 1);
    b.ri = (uint8_t)((ri_ok + MARGIN < USABLE) ? ri_ok + MARGIN : USABLE - 1);
    g_wi = b.wi; g_ri = b.ri; g_burst = SAFE_BURST;
    b.nb = (uint8_t)pick_burst();
    return true;
}

FLASHMEM static bool confirm(bank_t &b)
{
    for (int attempt = 0; attempt < 8; attempt++) {
        apply(b);
        fill(0, SPAN);
        uint32_t bad = 0;
        float rate = 0;
        for (int k = 0; k < CONFIRM_PASSES; k++) {
            const uint32_t t0 = ARM_DWT_CYCCNT;
            bad = verify(0, SPAN);
            const uint32_t cyc = ARM_DWT_CYCCNT - t0;
            if (k == 0 && bad == 0) rate = (float)(SPAN / ((double)cyc / F_CPU_ACTUAL) / 1e6);
            if (bad) break;
        }
        if (!bad) { b.rate = rate; return true; }
        say("        %lu wrong over %u kB at write %d read %d burst %d",
            (unsigned long)bad, SPAN / 1024, NOPS[b.wi], NOPS[b.ri], b.nb);
        if (b.ri + 1 < USABLE) b.ri++;
        else if (b.wi + 1 < USABLE) b.wi++;
        else return false;
        g_wi = b.wi; g_ri = b.ri; g_burst = SAFE_BURST;
        b.nb = (uint8_t)pick_burst();
    }
    return false;
}

FLASHMEM static bool qualify(bank_t &out)
{
    static const uint8_t SETUPS[2] = { 0, 16 };
    bank_t best;
    bool have = false;
    for (uint8_t mode = 0; mode < 3; mode++) {
        bool got = false;
        bank_t b = out;
        for (int s = 0; s < 2 && !got; s++) {
            b = out; b.mode = mode; b.su = SETUPS[s];
            if (!sweep(b)) continue;
            if (confirm(b)) got = true;
        }
        if (got) {
            say("      %-4s %-11s edge write %2d read %2d, running at write %2d read %2d burst %2d setup %2d -> %5.2f MB/s",
                out.name, MODE_NAME[mode], NOPS[b.ewi], NOPS[b.eri], NOPS[b.wi], NOPS[b.ri], b.nb, b.su, b.rate);
            if (!have || b.rate > best.rate) { best = b; have = true; }
            if (mode == 0) break;            /* quad cannot lose to one bit a clock */
        } else {
            say("      %-4s no %s setting survives %u kB", out.name, MODE_NAME[mode], SPAN / 1024);
        }
    }
    if (have) out = best;
    return have;
}

/* drive.py stable(): the same span read back again and again from one fill, every byte compared. */
FLASHMEM static bool stable(const bank_t &b)
{
    apply(b);
    fill(0, SPAN);
    for (int k = 0; k < STABLE_PASSES; k++) {
        const uint32_t bad = verify(0, SPAN);
        if (bad) { say("      %s: %lu wrong on stability pass %d; leaving it out", b.name, (unsigned long)bad, k + 1); return false; }
    }
    return true;
}

FLASHMEM static void psram_bring_up(void)
{
    say("\n  PSRAM discovery: asking all ten selects for an identity");
    static const struct { uint8_t kind, y; const char *name; } ROUTES[10] = {
        { SEL_CS0, 0, "CS0" }, { SEL_CS1, 0, "CS1" },
        { SEL_DEC, 0, "Y0" }, { SEL_DEC, 1, "Y1" }, { SEL_DEC, 2, "Y2" }, { SEL_DEC, 3, "Y3" },
        { SEL_DEC, 4, "Y4" }, { SEL_DEC, 5, "Y5" }, { SEL_DEC, 6, "Y6" }, { SEL_DEC, 7, "Y7" } };
    int ncand = 0;
    for (int r = 0; r < 10; r++) {
        uint8_t id[8] = { 0 };
        const bool ok = probe_id(ROUTES[r].kind, ROUTES[r].y, id);
        say("    %-4s %s  id %02X %02X", ROUTES[r].name, ok ? "PRESENT" : "absent ", id[0], id[1]);
        if (ok) {
            bank_t &b = cand[ncand++];
            memset(&b, 0, sizeof b);
            b.kind = ROUTES[r].kind; b.y = ROUTES[r].y;
            snprintf(b.name, sizeof b.name, "%s", ROUTES[r].name);
        }
    }
    say("  %d chips answered", ncand);

    say("\n  qualifying each: quad first, then single-bit, confirmed over %u kB four times", SPAN / 1024);
    int nq = 0;
    for (int i = 0; i < ncand; i++) {
        g_tag = (uint8_t)(i + 1);
        if (qualify(cand[i]) && stable(cand[i])) cand[nq++] = cand[i];
        else say("      %s: not usable", cand[i].name);
    }

    /* fastest first: the tokenizer sits at the bottom of the address space and is probed hardest */
    for (int i = 0; i < nq; i++)
        for (int j = i + 1; j < nq; j++)
            if (cand[j].rate > cand[i].rate) { bank_t t = cand[i]; cand[i] = cand[j]; cand[j] = t; }

    /* EVERY USABLE BYTE, before the model is allowed near it. All banks are filled first, each with its
     * own tag, and only then read back -- a bank that answers for another (a shared chip select, a
     * decoder line crossed) returns the wrong tag and shows up here, not as a wrong token an hour later. */
    say("\n  proving every byte: fill all %d banks (%d MB) with bank-tagged data, then read all back", nq, nq * 8);
    for (int i = 0; i < nq; i++) {
        apply(cand[i]);
        g_tag = (uint8_t)(0x40 + i);
        const uint32_t t0 = millis();
        fill(0, BANKSZ);
        say("    %-4s filled 8 MB in %.1f s", cand[i].name, (millis() - t0) / 1000.0);
    }
    nuse = 0;
    for (int i = 0; i < nq; i++) {
        apply(cand[i]);
        g_tag = (uint8_t)(0x40 + i);
        const uint32_t t0 = millis();
        const uint32_t bad = verify(0, BANKSZ);
        const float s = (millis() - t0) / 1000.0f;
        say("    %-4s %-11s read back 8 MB in %.1f s (%.2f MB/s): %lu wrong%s", cand[i].name,
            MODE_NAME[cand[i].mode], s, 8.388608f / s, (unsigned long)bad, bad ? "  -> DROPPED" : "");
        if (!bad) use[nuse++] = cand[i];
    }
    cur_bank = -1;
    say("  PSRAM in use: %d banks, %lu bytes", nuse, (unsigned long)nuse * BANKSZ);
}

/* ---------------------------------------------------------------------------------------------
 *  tl_plat.h for this board
 * ------------------------------------------------------------------------------------------ */
static SdFs     sd;
static FsFile   mf;
static uint64_t mf_size;

/* "::stop" is honoured at the next card read, i.e. BETWEEN two complete SdFat reads, never inside one.
 * A Teensy reset does not power-cycle the card: on 2026-09-27 a flash that rebooted the board mid-read left
 * the card answering CMD0 and CMD8 and never finishing ACMD41 (SdFat error 0x17) until power was cut.
 * tools/bench_run.py sends ::stop before it flashes. Any other line arriving during a run is dropped. */
static char     g_pend[64];
static int      g_pn;
static bool     g_abort;
static void poll_stop(void)
{
    while (Serial.available()) {
        const char ch = (char)Serial.read();
        if (ch == '\n' || ch == '\r') {
            g_pend[g_pn] = 0;
            if (!strcmp(g_pend, "::stop")) g_abort = true;
            g_pn = 0;
        } else if (g_pn < (int)sizeof g_pend - 1) {
            g_pend[g_pn++] = ch;
        }
    }
}

/* SEEKS MUST BE CHEAP, AND ON FAT32 THEY ARE NOT BY DEFAULT.
 *
 * SdFat's FatFile::seekSet walks the cluster chain from the file's FIRST cluster for every backward seek.
 * Measured 2026-09-27 12:00 (v5): a random 4 KB read cost 40 ms, and the batched prompt pass -- whose
 * feed-forward alternates 32 gate rows with 32 up rows, a backward seek every block, 12,384 a pass -- read
 * the card at 1.85 MB/s instead of 18.9: 992.9 s of card in a 1144 s pass whose arithmetic was 148 s.
 *
 * contiguousRange() walks the chain once and, when the file is one unbroken run of clusters (the normal
 * case for a file copied onto a fresh card), sets SdFat's contiguous flag: from then on seekSet is
 * firstCluster + n, no walk. If the file is fragmented, four read handles are kept and each read goes to the
 * one already positioned closest below it, so interleaved streams (gate/up) each move forward only. */
#define MF_POOL 4
static bool     mf_contig;
static uint32_t mf_sector0;                  /* the model file's first sector when it is contiguous */
static FsFile   mfp[MF_POOL];
static uint64_t mfp_pos[MF_POOL];
static uint32_t mfp_used[MF_POOL], mfp_tick;

/* THE CARD MUST BE IDLE WHENEVER THE BOARD IS. SdFat on a Teensy 4 in FIFO_SDIO mode leaves every read as an
 * open, paused CMD18 multi-block transfer (SdioTeensy.cpp readSector: "Infinite transfer", m_curState =
 * READ_STATE), closed only by the next non-sequential read or syncDevice() (CMD12). A reboot in that state hung
 * the card twice on 2026-09-27 -- 03:26, and 12:22 after a CLEAN ::stop between two reads -- each time until USB
 * power was cut. So every command the board handles ends with the transfer closed, and so does boot. */
static bool g_sd_up;
static void sd_idle(void) { if (g_sd_up) sd.card()->syncDevice(); }
/* the card's bus clock as SdFat set it (SdioCard; sd.card() is its base interface) */
static unsigned long sd_khz(void) { return (unsigned long)static_cast<SdioCard *>(sd.card())->kHzSdClk(); }

/* THE DMA PATH'S BURSTS (v8b). v7b measured DMA_SDIO at 7.26 MB/s against FIFO's 18.5 at the same 49.5 MHz
 * clock, flat across read sizes and target memories: a limit inside the controller's DMA, not the card.
 * SdFat's cardInit writes the whole watermark register, SDHC_WML = RDWML(16) | WRWML(16), leaving the read
 * burst length field at 0, and never sets PROT_CTRL's burst-length enables (bits 14:12). ::sdsweep tries
 * read watermark x read burst length x burst enables on DMA reads of the same 2 MB, checks every byte against
 * the FIFO read of it, and keeps the fastest that reads right. cardInit writes the register again on every
 * sd.begin, so the kept setting is reapplied after each (sd_cfg_reapply). */
static uint8_t g_rwml, g_rbrst, g_blen;
static bool    g_sdcfg;                      /* false: SdFat's own settings */
static bool    g_dma;                        /* the card came up in DMA_SDIO (see THE CARD AND THE ARITHMETIC) */
FLASHMEM static void sd_regs(const char *why)
{
    say("    uSDHC %s: WTMK_LVL 0x%08lX PROT_CTRL 0x%08lX MIX_CTRL 0x%08lX SYS_CTRL 0x%08lX VEND_SPEC 0x%08lX VEND_SPEC2 0x%08lX",
        why, (unsigned long)USDHC1_WTMK_LVL, (unsigned long)USDHC1_PROT_CTRL, (unsigned long)USDHC1_MIX_CTRL,
        (unsigned long)USDHC1_SYS_CTRL, (unsigned long)USDHC1_VEND_SPEC, (unsigned long)USDHC1_VEND_SPEC2);
}
static void sd_burst_apply(uint8_t wml, uint8_t brst, uint8_t blen)
{
    USDHC1_WTMK_LVL  = (USDHC1_WTMK_LVL & ~(0xFFu | (0x1Fu << 8))) | wml | ((uint32_t)brst << 8);
    USDHC1_PROT_CTRL = (USDHC1_PROT_CTRL & ~(7u << 12)) | ((uint32_t)blen << 12);
}
/* THE CARD'S CLOCK (v8b). FIFO reads run at 22.9 MB/s, 92.5% of what four data lines carry at 49.5 MHz; the
 * SD spec's next step (UHS-I) needs 1.8 V signalling, which this slot does not have. The uSDHC divides a
 * 198 MHz base (PLL2 PFD0 396 MHz / 2, SdFat's initClock); SdFat picks /4 = 49.5 MHz, the High Speed mode's
 * 50 MHz limit. /3 = 66 MHz and /2 = 99 MHz are beyond that spec. Every data block on the bus carries a CRC16
 * the controller checks, so a bad transfer fails the read instead of passing wrong bytes -- and ::sdclksweep
 * checks every byte anyway, against the same 8 MB read at 49.5 MHz. A divider that reads every byte right
 * twice over is kept (g_sddvs) and reapplied after every sd.begin. */
static uint8_t g_sddvs;                      /* 0: SdFat's own divider */
static unsigned long sd_clk_now(void)
{
    const uint32_t s = USDHC1_SYS_CTRL, fs = (s >> 8) & 0xFF, dvs = (s >> 4) & 0xF;
    return 198000ul / ((fs ? 2u * fs : 1u) * (dvs + 1u));
}
static bool sd_set_dvs(uint8_t dvs)
{
    USDHC1_SYS_CTRL = (USDHC1_SYS_CTRL & ~(0xFu << 4)) | ((uint32_t)dvs << 4);
    const uint32_t t0 = micros();
    while (!(USDHC1_PRES_STATE & (1u << 3)))                 /* SDSTB: the new clock is stable */
        if (micros() - t0 > 10000u) return false;
    return true;
}
static void sd_cfg_reapply(void)
{
    if (g_dma && g_sdcfg) sd_burst_apply(g_rwml, g_rbrst, g_blen);
    if (g_sddvs) sd_set_dvs(g_sddvs);
}

/* THE CARD AND THE ARITHMETIC AT THE SAME TIME (v7).
 *
 * In FIFO_SDIO mode the CPU itself copies every word out of the card controller and spins between sectors
 * (SdioTeensy.cpp readData: waitTimeout, no yield) -- a pass is card time PLUS arithmetic time. In DMA_SDIO mode
 * the controller writes the bytes by itself and SdFat waits in yieldTimeout(), calling yield() until the
 * transfer is done. That wait is where the core's pending rows run: yield() below works ONE row per call by
 * default (::slice 0), so SdFat looks at the transfer again after every row and the card is never left idle
 * behind a slice of arithmetic; ::slice N works rows for N us per call instead, for the A/B. One row is at
 * most a few ms even batched, far inside SdFat's 1 s busy timeout. Rows it does not reach are worked by the
 * core after the read, so the result is the same either way (PC-proven with every interleaving).
 * The core's row buffers are in DTCM, which the controller's DMA reaches and the cache does not cover.
 *
 * Arm the work only around the card read itself: Serial.available() calls yield() whenever nothing has
 * arrived (cores/teensy4/usb_serial.c), so a poll_stop() with the work armed would run rows BEFORE the
 * transfer starts -- no overlap, but counted as hidden. plat_sd_read_overlap polls first, then arms. */
/* g_dma: declared with the SD globals above */
static int  g_overlap = 1;                   /* ::overlap 0|1 */
static int  g_align = 1;                     /* ::align 0|1: whole sectors per pipeline read, DMA only */
static uint32_t g_slice_us;                  /* ::slice N: 0 = one row per yield() */
static int  g_sdpath;                        /* ::sdpath file|adma: 1 = whole-sector pipeline reads by ADMA2 (v9b) */
static void adma_arm(void *buf, uint32_t n);
static void adma_disarm(void);
static int (*volatile g_ywork)(void *);
static void *g_yctx;

void yield(void)                             /* replaces the core's weak yield (it does nothing in this sketch) */
{
    int (*w)(void *) = g_ywork;
    if (!w) return;
    const uint32_t t0 = micros();
    do {
        if (!w(g_yctx)) { g_ywork = 0; return; }
    } while (micros() - t0 < g_slice_us);
}

/* WHICH MODEL: qwen3b.gguf unless model.txt on the card names another file (written by ::model NAME,
 * which then restarts the board). The tokenizer store is the same name with .tok. */
static char g_model_name[48] = "qwen3b.gguf";
static char g_tok_name[48]   = "qwen3b.tok";
FLASHMEM static void model_name_load(void)
{
    FsFile f = sd.open("model.txt", O_RDONLY);
    if (!f) return;
    char b[48];
    const int n = f.read(b, sizeof b - 1);
    f.close();
    if (n <= 0) return;
    b[n] = 0;
    for (int i = 0; b[i]; i++) if (b[i] == '\r' || b[i] == '\n' || b[i] == ' ') { b[i] = 0; break; }
    const size_t L = strlen(b);
    if (L < 6 || L > 40 || strcmp(b + L - 5, ".gguf") || !sd.exists(b)) {
        say("    model.txt names %s: not on the card, opening %s instead", b, g_model_name);
        return;
    }
    strcpy(g_model_name, b);
    strcpy(g_tok_name, b);
    strcpy(g_tok_name + L - 5, ".tok");
    say("    model.txt: %s (tokenizer store %s)", g_model_name, g_tok_name);
}

/* the model file open, and whether its clusters are one run (then every seek is one addition) */
FLASHMEM static bool model_file_open(void)
{
    mf = sd.open(g_model_name, O_RDONLY);
    if (!mf) return false;
    mf_size = mf.fileSize();
    say("    %s: %llu bytes", g_model_name, (unsigned long long)mf_size);
    uint32_t s0 = 0, s1 = 0;
    const uint32_t t0 = millis();
    mf_contig = mf.contiguousRange(&s0, &s1);
    mf_sector0 = s0;
    const uint8_t ft = sd.fatType();
    if (mf_contig) {
        say("    contiguous on the card, sectors %lu..%lu (%s): every seek is one addition (checked in %lu ms)",
            (unsigned long)s0, (unsigned long)s1, ft == FAT_TYPE_EXFAT ? "exFAT" : "FAT32", (unsigned long)(millis() - t0));
    } else {
        for (int i = 0; i < MF_POOL; i++) { mfp[i] = sd.open(g_model_name, O_RDONLY); mfp_pos[i] = 0; mfp_used[i] = 0; }
        say("    FRAGMENTED on the card (%s): backward seeks walk the cluster chain; %d forward-moving read handles instead",
            ft == FAT_TYPE_EXFAT ? "exFAT" : "FAT32", MF_POOL);
    }
    return true;
}

/* THE TOKENIZER'S STORE (tl_plat.h): qwen3b.tok beside the model, created by the core from the model file
 * the first time and reused while it matches. Created as ONE contiguous run (preAllocate), and reopened
 * with contiguousRange(), because the tokenizer reads it at random and a FAT32 seek in a fragmented file
 * walks the cluster chain (40 ms a read, measured 2026-09-27). */
static FsFile tf;
static bool   tf_open;
FLASHMEM int plat_tok_open(uint32_t bytes)
{
    if (tf_open) { tf.close(); tf_open = false; }
    if (!bytes) {
        tf = sd.open(g_tok_name, O_RDWR);
        if (!tf) return -1;
        uint32_t s0 = 0, s1 = 0;
        const bool contig = tf.contiguousRange(&s0, &s1);
        say("    %s: %lu bytes on the card%s", g_tok_name, (unsigned long)tf.fileSize(), contig ? ", contiguous" : ", FRAGMENTED (slow lookups)");
        tf_open = true;
        return 0;
    }
    if (sd.exists(g_tok_name)) sd.remove(g_tok_name);
    tf = sd.open(g_tok_name, O_RDWR | O_CREAT | O_TRUNC);
    if (!tf) return -1;
    if (!tf.preAllocate(bytes)) {
        say("    %s: no contiguous %lu bytes free on the card", g_tok_name, (unsigned long)bytes);
        tf.close(); sd.remove(g_tok_name);
        return -1;
    }
    say("    %s: %lu bytes allocated on the card in one run; writing the tokenizer's tables", g_tok_name, (unsigned long)bytes);
    tf_open = true;
    return 0;
}
int plat_tok_read(uint32_t a, void *dst, uint32_t n)
{
    if (!tf_open || !tf.seekSet(a)) return -1;
    return tf.read(dst, n) == (int)n ? 0 : -1;
}
int plat_tok_write(uint32_t a, const void *src, uint32_t n)
{
    if (!tf_open || !tf.seekSet(a)) return -1;
    return tf.write(src, n) == n ? 0 : -1;
}
int plat_tok_flush(void) { return (tf_open && tf.sync()) ? 0 : -1; }

/* ::sdio fifo|dma -- restart the card in the other mode, for an A/B on one build: close every handle, close
 * the transfer (sd_idle: the card is left idle, never mid-transfer), end, begin in the new mode, reopen. */
FLASHMEM static bool sdio_switch(bool dma)
{
    sd_idle();
    const bool had_tok = tf_open;
    if (tf_open) { tf.close(); tf_open = false; }
    mf.close();
    for (int i = 0; i < MF_POOL; i++) if (mfp[i]) mfp[i].close();
    sd.end();
    g_sd_up = false;
    if (!sd.begin(SdioConfig(dma ? DMA_SDIO : FIFO_SDIO))) {
        say("  SD restart in %s failed: SdFat error 0x%02X data 0x%08lX", dma ? "DMA_SDIO" : "FIFO_SDIO",
            (unsigned)sd.sdErrorCode(), (unsigned long)sd.sdErrorData());
        return false;
    }
    g_sd_up = true;
    g_dma = dma;
    sd_cfg_reapply();
    say("    SD up (%s), clock %lu kHz", dma ? "DMA_SDIO" : "FIFO_SDIO", sd_clk_now());
    if (!model_file_open()) return false;
    return !had_tok || plat_tok_open(0) == 0;
}

static int mf_read_at(uint64_t off, void *dst, uint32_t n, uint32_t need);

/* A pipeline read (tl_plat.h). In DMA mode with ::align 1 it reads the whole sectors around the block into
 * the room the core leaves for them: one CMD18 for the block. Unaligned, SdFat splits it into a head sector
 * and a tail sector through its cache and a middle by DMA -- two or three card commands, each with the card's
 * own start-up time. FIFO mode keeps one transfer open across reads, so there it reads exactly the block
 * (whole sectors would re-read each boundary sector and restart the transfer). */
int plat_sd_read_overlap(uint64_t off, void *dst, uint32_t n, int (*work)(void *), void *ctx)
{
    poll_stop();                             /* before the work is armed: Serial.available() calls yield() */
    if (g_abort) return -1;
    uint64_t a = off;
    uint8_t *d = (uint8_t *)dst;
    uint32_t len = n;
    if (g_dma && g_align) {
        const uint32_t pre = (uint32_t)(off & 511u);
        a -= pre;
        d -= pre;
        len = (pre + n + 511u) & ~511u;
    }
    if (work && g_dma && g_overlap) { g_yctx = ctx; g_ywork = work; }
    /* ::sdpath adma (v9b): whole sectors of the contiguous model file straight through the card object with an
     * ADMA2 descriptor table for this very buffer -- no file layer, no cluster splitting, one CMD18 -- and the
     * core's rows run in SdFat's wait for it, exactly as with simple DMA. The pipeline buffers are DTCM. */
    if (g_sdpath == 1 && g_dma && mf_contig && (a % 512u) == 0 && (len % 512u) == 0 && len <= 8u * 32768u &&
        ((uintptr_t)d % 4u) == 0 && a + len <= ((mf_size + 511u) & ~511ull)) {
        adma_arm(d, len);
        const bool ok = sd.card()->readSectors((uint32_t)(mf_sector0 + a / 512u), d, len / 512u);
        adma_disarm();
        g_ywork = 0;
        if (!ok) { say("    ADMA2 read failed at %llu (+%lu): SdFat error 0x%02X", (unsigned long long)a, (unsigned long)len, (unsigned)sd.sdErrorCode()); return -1; }
        return 0;
    }
    const int r = mf_read_at(a, d, len, (uint32_t)(off - a) + n);   /* the file may end inside the last sector */
    g_ywork = 0;
    return r;
}

int plat_sd_read(uint64_t off, void *dst, uint32_t n)
{
    poll_stop();
    if (g_abort) return -1;
    return mf_read_at(off, dst, n, n);
}

/* n bytes at off into dst; succeeds when at least `need` of them arrived */
/* A card read above SdFat's clock that fails (the controller checks every block's CRC16, so a marginal clock
 * shows up as a failed read, not as wrong bytes) drops the card back to SdFat's 49.5 MHz for good, restarts
 * it, and reads again. */
static int mf_read_once(uint64_t off, void *dst, uint32_t n, uint32_t need);
static int mf_read_at(uint64_t off, void *dst, uint32_t n, uint32_t need)
{
    const int r = mf_read_once(off, dst, n, need);
    if (!r || !g_sddvs) return r;
    say("    card read failed at %lu kHz: back to SdFat's clock, card restarted, read again", sd_clk_now());
    g_sddvs = 0;
    if (!sdio_switch(g_dma)) return -1;
    return mf_read_once(off, dst, n, need);
}
static int mf_read_once(uint64_t off, void *dst, uint32_t n, uint32_t need)
{
    if (mf_contig) {
        if (!mf.seekSet(off)) return -1;
        return (mf.read(dst, n) >= (int)need) ? 0 : -1;
    }
    int h = -1;
    for (int i = 0; i < MF_POOL; i++)                      /* nearest handle at or below off: a forward seek */
        if (mfp[i] && mfp_pos[i] <= off && (h < 0 || mfp_pos[i] > mfp_pos[h])) h = i;
    if (h < 0)                                            /* none below: the least recently used one */
        for (int i = 0; i < MF_POOL; i++)
            if (mfp[i] && (h < 0 || mfp_used[i] < mfp_used[h])) h = i;
    if (h < 0) return -1;
    if (!mfp[h].seekSet(off)) return -1;
    const int got = mfp[h].read(dst, n);
    mfp_pos[h] = off + (uint64_t)(got > 0 ? got : 0);
    mfp_used[h] = ++mfp_tick;
    return got >= (int)need ? 0 : -1;
}

/* An extended ACMD41 wait was tried here on 2026-09-27 and REMOVED: its raw command path read a stale
 * response register (CMD8 'answer' 0x00000120 was an earlier CMD55's R1 status), so its result proved
 * nothing. SdFat's own eight attempts are the evidence: CMD0 and CMD8 answer, ACMD41 never reports ready
 * -- the card is hung, and on a Teensy 4.1 only cutting its power (USB) clears that. */
uint64_t plat_sd_size(void) { return mf_size; }
/* THE CHIPS ARE OFFERED AS THEY ARE (tl_plat.h, v9): nuse independent banks, fastest first, never one flat
 * memory. Selecting a chip is a bus reconfiguration and a chip reset (apply: two 5 us waits, a 2 ms wait and a
 * 50 us wait), so it is counted and timed; the core's per-layer layout exists to make it rare. */
int      plat_ps_banks(void) { return nuse; }
uint32_t plat_ps_bank_bytes(int b) { return (b >= 0 && b < nuse) ? BANKSZ : 0; }
static uint32_t ps_switches;
static uint64_t ps_switch_us;
static inline int ps_bank(int bi)
{
    if (bi < 0 || bi >= nuse) return -1;
    if (bi != cur_bank) {
        const uint32_t t0 = micros();
        apply(use[bi]); cur_bank = bi;
        ps_switch_us += micros() - t0;
        ps_switches++;
    }
    return bi;
}
/* A WRITE CHECKS ITSELF; A READ IS CHECKED BY THE CORE.
 *
 * A write is read back and compared, and re-done until it reads back right. A read is ONE transfer: the core
 * keeps a checksum beside every cache row and re-reads a row that fails it (tl_core.c cache_read), so the
 * double read v5-v8 did here is gone -- half the PSRAM read traffic for the same detection. Every write
 * correction is counted and the first few are logged with their place, so a marginal bank shows up as numbers
 * in the log instead of as a wrong word the model trips over later.
 *
 * What neither can catch is a read that is wrong the SAME way every time; that is what MARGIN is for. */
static uint8_t  vchk[BLK] __attribute__((aligned(32)));
static uint32_t ps_fix_r, ps_fix_w, ps_bad, ps_events;   /* ps_fix_r: the core's row re-reads, added per pass */

static void ps_event(const char *what, int b, uint32_t off, uint32_t n, uint32_t nbytes)
{
    if (ps_events++ < 16)
        say("    PSRAM self-check: %s on bank %d (%s) at %lu (+%lu), %lu byte(s) differed", what, b, use[b].name,
            (unsigned long)off, (unsigned long)n, (unsigned long)nbytes);
}

static int ps_write_chunk(int b, uint32_t off, const uint8_t *s, uint32_t k)   /* k <= BLK */
{
    for (int attempt = 0; attempt < 4; attempt++) {
        do_write(off, s, k);
        do_read(off, vchk, k);
        if (!memcmp(s, vchk, k)) {
            if (attempt) { ps_fix_w++; ps_event("write re-done", b, off, k, (uint32_t)attempt); }
            return 0;
        }
    }
    ps_bad++;
    ps_event("write never read back right", b, off, k, 4);
    return -1;
}

int plat_ps_read(int b, uint32_t off, void *dst, uint32_t n)
{
    if ((uint64_t)off + n > BANKSZ || ps_bank(b) < 0) return -1;
    uint8_t *d = (uint8_t *)dst;
    while (n) {
        const uint32_t k = n < BLK ? n : BLK;
        do_read(off, d, k);
        off += k; d += k; n -= k;
    }
    return 0;
}
int plat_ps_write(int b, uint32_t off, const void *src, uint32_t n)
{
    if ((uint64_t)off + n > BANKSZ || ps_bank(b) < 0) return -1;
    const uint8_t *s = (const uint8_t *)src;
    while (n) {
        const uint32_t k = n < BLK ? n : BLK;
        if (ps_write_chunk(b, off, s, k)) return -1;
        off += k; s += k; n -= k;
    }
    return 0;
}
double plat_now(void)
{
    static uint32_t last;
    static uint64_t hi;
    const uint32_t m = micros();
    if (m < last) hi += 1ull << 32;
    last = m;
    return (double)(hi + m) * 1e-6;
}
void plat_log(const char *fmt, ...)
{
    char b[240];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    Serial.println(b);
}
void plat_emit(const char *s, int n) { Serial.write((const uint8_t *)s, (size_t)n); }

/* ---------------------------------------------------------------------------------------------
 *  the run, and what a host can send (one line each, USB serial)
 *
 *    I              identity: "I bench-one psram_llm v9 ..." -- tools/bench_run.py asks this before flashing
 *    ::gen N        tokens to generate after the prompt, 1..512 (default 16)
 *    ::bench        component benchmarks: the card at three read sizes and at random, the M7 kernels on
 *                   real Q4_K and Q6_K rows held in RAM, raw and self-checked PSRAM on a free bank
 *    ::stats        banks in use, PSRAM self-check counters, temperature
 *    ::hold         nothing -- a host sends it during the boot window so the default prompt does not run
 *    ::prefill 0|1  prompt through tl_prefill (default 1) or one token per pass, for A/B runs on one board
 *    ::multi K      the next K prompt lines (1..8) are answered TOGETHER: every pass over the weights feeds
 *                   all unfinished prompts (tl_multi_pass), ::gen tokens each; "::multi 0" cancels
 *    ::share 0|1    in a ::multi, a prompt that opens with the same tokens as an earlier one (every chat
 *                   prompt's system turn) copies those cache rows instead of computing them (default 1)
 *    ::sdio fifo|dma  restart the card in FIFO_SDIO (v6's way) or DMA_SDIO (the default from v7)
 *    ::overlap 0|1  DMA only: the arithmetic runs while the card reads (default 1)
 *    ::align 0|1    DMA only: a pipeline read takes the whole sectors around its block, one command (default 1)
 *    ::slice N      rows per yield(): 0 = one row (default), N = rows for N us per call
 *    ::sdsweep      the DMA path at every read watermark / burst length / burst enable, each 2 MB checked
 *                   byte for byte against FIFO; keeps the fastest that reads right ("SWEEP ..." lines)
 *    ::sdcfg W B L  DMA read watermark W words, burst B words, burst enables L; "::sdcfg 0" = SdFat's own
 *    ::sdregs       the SD controller's registers
 *    ::sdclksweep   the card clock above SdFat's 49.5 MHz (66, 99), 8 MB checked byte for byte at each;
 *                   keeps the fastest that reads right ("CLKSWEEP ..." lines)
 *    ::sdclk N      card clock = 198 MHz / N (2..16); "::sdclk 0" = SdFat's own
 *    ::admamin N    a pass with N or more positions reads the card by ADMA2 with the arithmetic under the read,
 *                   a smaller one by FIFO (default 3, measured: docs/56); 0 = no automatic switching
 *    ::sdpath file|adma  a pipeline read goes through SdFat's file layer (default) or, in DMA mode, straight to
 *                   the card object as whole sectors with an ADMA2 descriptor table for the core's buffer
 *    ::sdadma       the card by ADMA2 (descriptor DMA) at four read sizes, 2 MB each checked byte for byte
 *                   against FIFO and against SdFat's simple DMA ("ADMA ..." lines)
 *                   (every one of these four gives the same step lines; they move only the time)
 *    anything else  a prompt. \n \t \\ are unescaped, so code and chat templates fit on one line
 *
 *  Machine-readable lines for tests/psram_llm/suite.py: "step ...", "S<k> step ..." (prompt k of a ::multi),
 *  "STAGES ...", "RUN ...", "BENCH ...".
 * ------------------------------------------------------------------------------------------ */
#define MAX_GEN 512
static int      g_gen = 16;
/* The card test and the benchmarks borrow the core's arena between passes (tl_scratch) rather than keep
 * their own 119 KB of OCRAM, which the batched prompt path needs. */
#define SDBUF_BYTES (64u * 1024u)
static uint8_t  *sdbuf;
static float    *bx;
static int8_t   *bxq;
static uint32_t *bxw;                        /* the activation vector pre-widened (BENCH path widened) */
FLASHMEM static bool scratch_init(void)
{
    uint32_t n = 0;
    uint8_t *s = (uint8_t *)tl_scratch(&n);
    if (n < SDBUF_BYTES + 11008u * 4u + 11008u + 11008u * 2u) return false;
    sdbuf = s;
    bx  = (float *)(s + SDBUF_BYTES);
    bxq = (int8_t *)(s + SDBUF_BYTES + 11008u * 4u);
    bxw = (uint32_t *)(s + SDBUF_BYTES + 11008u * 4u + 11008u);
    return true;
}
static float    bxs[11008 / 32 + 1];
static int32_t  bxsum[11008 / 32 + 1];
static tl_info_t info;
static bool ready;

static void halt(const char *why)
{
    say("\n  HALTED: %s", why);
    ready = false;
}

static void esc(const char *s, int n, char *o, int omax)
{
    int k = 0;
    for (int i = 0; i < n && k < omax - 5; i++) {
        const unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') { o[k++] = '\\'; o[k++] = (char)c; }
        else if (c >= 0x20 && c < 0x7F) o[k++] = (char)c;
        else k += snprintf(o + k, (size_t)(omax - k), "\\x%02X", c);
    }
    o[k] = 0;
}

static void unescape(char *s)
{
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '\\' && p[1]) {
            p++;
            if (*p == 'n') *o++ = '\n';
            else if (*p == 't') *o++ = '\t';
            else if (*p == '\\') *o++ = '\\';
            else { *o++ = '\\'; *o++ = *p; }
        } else {
            *o++ = *p;
        }
    }
    *o = 0;
}

static bool read_line(char *out, int max, uint32_t wait_ms)
{
    int n = 0;
    const uint32_t t0 = millis();
    while (millis() - t0 < wait_ms) {
        while (Serial.available()) {
            const char ch = (char)Serial.read();
            if (ch == '\n' || ch == '\r') { if (n) { out[n] = 0; return true; } }
            else if (n < max - 1) out[n++] = ch;
        }
        delay(5);
    }
    out[n] = 0;
    return n > 0;
}

static int g_prefill = 1;                 /* ::prefill 0|1 -- the prompt through tl_prefill, or one token at a time */

/* One position's line, exactly the format tests/tl_host.c and tests/tl_ref.c print, and the text so far. */
/* WHICH WAY THE CARD IS READ IS DECIDED PER PASS (v9c). Measured 2026-09-28 (docs/56): a batched pass of 5
 * positions took 208.6 s by FIFO and 143.0 s by ADMA2 with the arithmetic run under the read; a one-token pass
 * 104.8 s by FIFO and 137.5 s by ADMA2, because 29 s of arithmetic cannot hide 131 s of card. So a pass with
 * g_adma_min or more positions reads the card by ADMA2 with the overlap, a smaller one by FIFO; the switch
 * is a card restart (about 2 s), made only when the answer changes -- once per prompt, a few per group. */
static int g_adma_min = 3;                   /* ::admamin N; 0 = never switch automatically */
static void card_for_slots(int slots)
{
    if (!mf_contig || g_adma_min <= 0) return;
    const bool want = slots >= g_adma_min;
    if (want != g_dma) {
        say("  card: %d position%s in this pass -> %s", slots, slots == 1 ? "" : "s",
            want ? "ADMA2 with the arithmetic under the read" : "FIFO");
        if (!sdio_switch(want)) { halt("the card did not restart; power-cycle the Teensy (unplug USB for 2 s)"); return; }
        say("SDIO %s", want ? "dma" : "fifo");
    }
    if (g_sdpath != (want ? 1 : 0)) { g_sdpath = want ? 1 : 0; say("SDPATH %s", want ? "adma" : "file"); }
    if (want) { g_overlap = 1; g_align = 1; }
}

/* After a chip fault a prompt is re-run from position 0 (run_prompt, run_multi below): the step lines that
 * were already printed come out again prefixed "R ", so the log holds both copies to compare. */
static int g_replay_to, g_run_steps;                 /* one prompt: steps printed before the fault / so far */
static int g_multi_replay[8], g_multi_printed[8];    /* MULTI_MAX prompts: positions printed, per sequence */

static void emit_step(const char *prompt, int n, int step, int32_t fed, int32_t t1, float l1, int32_t t2, float l2,
                      char *gen, int *gl, int genmax)
{
    static char txt[256], e[1024];
    const int k = tl_decode(t1, txt, (int)sizeof txt - 1);
    esc(txt, k, e, (int)sizeof e);
    char line[1400];
    snprintf(line, sizeof line, "%sstep %d pos %d fed %ld -> top1 %ld %.9g top2 %ld %.9g text \"%s\"",
             step < g_replay_to ? "R " : "", step, step, (long)fed, (long)t1, (double)l1, (long)t2, (double)l2, e);
    if (step + 1 > g_run_steps) g_run_steps = step + 1;
    Serial.println(line);
    if (step >= n - 1 && *gl + k < genmax) { memcpy(gen + *gl, txt, (size_t)k); *gl += k; }
    if (step >= n - 1) {
        Serial.print("  >> ");
        Serial.write((const uint8_t *)prompt, strlen(prompt));
        Serial.write((const uint8_t *)gen, (size_t)*gl);
        Serial.println();
    }
}

static void pass_lines(const char *what, const tl_stats_t &st, float tc)
{
    static uint32_t sw_prev;
    static uint64_t swus_prev;
    say("    %s: %.1f s: SD %.1f MB in %.1f s (%.2f MB/s), PSRAM %.0f KB read %.1f KB written in %.2f s, compute %.1f s;"
        " PSRAM self-check so far: %lu rows re-read, %lu writes re-done, %lu unresolved; chip selects this pass %lu (%.3f s)", what,
        st.t_total, st.sd_bytes / 1048576.0, st.t_sd, st.sd_bytes / 1048576.0 / (st.t_sd > 0 ? st.t_sd : 1),
        st.ps_read / 1024.0, st.ps_written / 1024.0, st.t_ps, st.t_compute,
        (unsigned long)ps_fix_r, (unsigned long)ps_fix_w, (unsigned long)ps_bad,
        (unsigned long)(ps_switches - sw_prev), (ps_switch_us - swus_prev) / 1e6);
    sw_prev = ps_switches; swus_prev = ps_switch_us;
    say("STAGES %s embed=%.3f qkv=%.2f att=%.3f wo=%.2f ffn=%.2f head=%.2f hidden=%.2f temp_c=%.1f",
        what, st.t_embed, st.t_qkv, st.t_att, st.t_wo, st.t_ffn, st.t_head, st.t_hidden, tc);
}

static bool run_prompt_once(const char *prompt)
{
    g_abort = false;                      /* a ::stop that arrived while idle is not a stop for this run */
    static int32_t ids[1024];
    const uint32_t sw0 = ps_switches;
    const uint64_t swus0 = ps_switch_us;
    const int n = tl_encode(prompt, ids, 1024);
    if (n <= 0) { say("  prompt did not encode"); say("RUN error=encode"); return true; }
    if (n + g_gen > info.max_seq) { say("  prompt too long for the %d-position cache", info.max_seq); say("RUN error=too_long"); return true; }
    {
        static char ep[4096];
        esc(prompt, (int)strlen(prompt), ep, (int)sizeof ep);
        say("\n  prompt: \"%s\"  (%d tokens, generating %d, prompt %s)", ep, n, g_gen,
            g_prefill ? "batched through tl_prefill" : "one token per pass");
    }
    Serial.print("prompt ids:");
    for (int i = 0; i < n; i++) { Serial.print(' '); Serial.print((long)ids[i]); }
    Serial.println();

    static char gen[8192];
    int gl = 0, passes = 0, step0 = 0;
    int32_t next = 0;
    double total = 0, t_sd = 0, t_ps = 0, t_cmp = 0, t_hid = 0, t_prompt = 0;
    uint64_t sd_total = 0;
    float tmax = -1000.0f;
    bool ended = false;

    if (g_prefill) {
        static int32_t a1[64], a2[64];
        static float   b1[64], b2[64];
        const int B = tl_batch() < 64 ? tl_batch() : 64;
        for (int p0 = 0; p0 < n; p0 += B) {
            const int k = (n - p0) < B ? (n - p0) : B;
            card_for_slots(k);
            tl_stats_reset();
            if (tl_prefill(ids + p0, k, p0, a1, b1, a2, b2)) {
                if (g_abort) { say("  STOPPED in the prompt by ::stop, between two card reads"); say("RUN error=stopped step=%d", p0); g_abort = false; return true; }
                say("  prefill failed at positions %d..%d: %s", p0, p0 + k - 1, tl_last_error());
                if (tl_fault_bank() >= 0) return false;
                say("RUN error=prefill step=%d", p0);
                return true;
            }
            passes++;
            tl_stats_t st;
            tl_stats(&st);
            ps_fix_r += st.ps_rows_reread; ps_bad += st.ps_rows_bad;
            total += st.t_total; t_sd += st.t_sd; t_ps += st.t_ps; t_cmp += st.t_compute; t_hid += st.t_hidden; t_prompt += st.t_total;
            sd_total += st.sd_bytes;
            const float tc = tempmonGetTemp();
            if (tc > tmax) tmax = tc;
            for (int j = 0; j < k; j++) emit_step(prompt, n, p0 + j, ids[p0 + j], a1[j], b1[j], a2[j], b2[j], gen, &gl, (int)sizeof gen);
            char what[48];
            snprintf(what, sizeof what, "prefill=%d..%d", p0, p0 + k - 1);
            pass_lines(what, st, tc);
            if (p0 + k == n) next = a1[k - 1];
        }
        step0 = n;
        if (next == info.eos) ended = true;
    }
    if (!ended && step0 < n + g_gen) card_for_slots(1);
    for (int step = step0; !ended && step < n + g_gen; step++) {
        const int32_t fed = step < n ? ids[step] : next;
        int32_t t1, t2;
        float l1, l2;
        tl_stats_reset();
        if (tl_forward(fed, step, &t1, &l1, &t2, &l2)) {
            if (g_abort) {
                say("  STOPPED at step %d by ::stop, between two card reads", step);
                say("RUN error=stopped step=%d", step);
                g_abort = false;
                return true;
            }
            say("  forward failed at step %d: %s", step, tl_last_error());
            if (tl_fault_bank() >= 0) return false;
            say("RUN error=forward step=%d", step);
            return true;
        }
        passes++;
        tl_stats_t st;
        tl_stats(&st);
        ps_fix_r += st.ps_rows_reread; ps_bad += st.ps_rows_bad;
        total += st.t_total; t_sd += st.t_sd; t_ps += st.t_ps; t_cmp += st.t_compute; t_hid += st.t_hidden;
        if (step < n) t_prompt += st.t_total;
        sd_total += st.sd_bytes;
        const float tc = tempmonGetTemp();
        if (tc > tmax) tmax = tc;
        emit_step(prompt, n, step, fed, t1, l1, t2, l2, gen, &gl, (int)sizeof gen);
        char what[24];
        snprintf(what, sizeof what, "step=%d", step);
        pass_lines(what, st, tc);
        next = t1;
        if (step >= n - 1 && t1 == info.eos) break;
    }
    gen[gl] = 0;
    static char ea[8192 * 2];
    esc(gen, gl, ea, (int)sizeof ea);
    say("\n  ANSWER: \"%s\"", ea);
    say("  %d passes over the weights in %.0f s (prompt %.0f s), %.1f s each; %.2f GB read from the card", passes, total,
        t_prompt, total / (passes ? passes : 1), sd_total / 1073741824.0);
    say("  PSRAM self-check over the whole run: %lu rows re-read, %lu writes re-done, %lu unresolved",
        (unsigned long)ps_fix_r, (unsigned long)ps_fix_w, (unsigned long)ps_bad);
    say("RUN n_prompt=%d n_gen=%d prefill=%d passes=%d total_s=%.1f prompt_s=%.1f per_pass_s=%.2f sd_gb=%.3f sd_mbs=%.2f"
        " sd_s=%.1f compute_s=%.1f hidden_s=%.1f ps_s=%.1f ps_fix_r=%lu ps_fix_w=%lu ps_bad=%lu ps_sw=%lu ps_sw_s=%.2f temp_max_c=%.1f",
        n, g_gen, g_prefill, passes, total, t_prompt, total / (passes ? passes : 1), sd_total / 1073741824.0,
        sd_total / 1048576.0 / (t_sd > 0 ? t_sd : 1), t_sd, t_cmp, t_hid, t_ps,
        (unsigned long)ps_fix_r, (unsigned long)ps_fix_w, (unsigned long)ps_bad,
        (unsigned long)(ps_switches - sw0), (ps_switch_us - swus0) / 1e6, tmax);
    say("  DONE. Type another prompt and press Enter.");
    return true;
}

/* A PSRAM chip that fails mid-run: the core retires it and moves its layers to spare slots on the others
 * (tl_bank_fault); the prompt is then re-run from position 0, which recomputes everything exactly. The step
 * lines already printed are printed again with an "R " prefix (suite.py counts only "step" lines; the R
 * lines are there to be compared with them in the log). Once per prompt: a second chip failing ends it. */
static void run_prompt(const char *prompt)
{
    g_replay_to = 0; g_run_steps = 0;
    if (run_prompt_once(prompt)) return;
    const int fb = tl_fault_bank();
    if (fb < 0) { say("RUN error=failed"); return; }
    if (tl_bank_fault(fb)) {
        say("  chip %s (bank %d) failed and cannot be replaced: %s", use[fb].name, fb, tl_last_error());
        say("RUN error=bank_fault bank=%d", fb);
        return;
    }
    say("  chip %s (bank %d) failed: retired; its layers are now on other chips; re-running the prompt from position 0",
        use[fb].name, fb);
    say("FAULT bank=%d chip=%s steps_printed=%d", fb, use[fb].name, g_run_steps);
    g_replay_to = g_run_steps; g_run_steps = 0;
    if (!run_prompt_once(prompt)) { say("  a second chip failed: %s", tl_last_error()); say("RUN error=bank_fault_again"); }
}

/* ::multi -- several prompts, one pass over the weights for all of them. Three quarters of a pass is the card
 * read, which one prompt or eight cost the same; each extra prompt adds only its arithmetic. */
#define MULTI_MAX 8
#define MULTI_IDS 1024
static int g_multi_want = 0, g_multi_n = 0;
DMAMEM static int32_t mids[MULTI_MAX][MULTI_IDS];
static tl_seq_t mseq[MULTI_MAX];

static void multi_emit(void *ctx, int s, int pos, int32_t fed, int32_t t1, float l1, int32_t t2, float l2)
{
    (void)ctx;
    static char txt[256], e[1024], line[1400];
    const int k = tl_decode(t1, txt, (int)sizeof txt - 1);
    esc(txt, k, e, (int)sizeof e);
    snprintf(line, sizeof line, "%sS%d step %d pos %d fed %ld -> top1 %ld %.9g top2 %ld %.9g text \"%s\"",
             pos < g_multi_replay[s] ? "R " : "", s, pos, pos, (long)fed, (long)t1, (double)l1, (long)t2, (double)l2, e);
    if (pos + 1 > g_multi_printed[s]) g_multi_printed[s] = pos + 1;
    Serial.println(line);
}

static bool run_multi_once(int ns)
{
    g_abort = false;
    const uint32_t sw0 = ps_switches;
    const uint64_t swus0 = ps_switch_us;
    if (tl_multi_begin(mseq, ns)) {
        say("  multi: %s", tl_last_error());
        if (tl_fault_bank() >= 0) return false;
        say("RUN error=multi");
        return true;
    }
    say("\n  answering %d prompts together, %d tokens each", ns, g_gen);
    int shared = 0;
    for (int i = 0; i < ns; i++)
        if (mseq[i].donor >= 0) {
            say("  S%d copies its first %d positions from S%d (the same opening tokens)", i, mseq[i].share, mseq[i].donor);
            shared += mseq[i].share;
        }
    int passes = 0, fed = 0, r;
    double total = 0, t_sd = 0, t_ps = 0, t_cmp = 0, t_hid = 0;
    uint64_t sd_total = 0;
    float tmax = -1000.0f;
    for (;;) {
        {
            int live = 0;
            for (int i = 0; i < ns; i++) live += !mseq[i].done;
            if (live) card_for_slots(live < tl_batch() ? live : tl_batch());
        }
        tl_stats_reset();
        r = tl_multi_pass(mseq, ns, multi_emit, NULL);
        if (r <= 0) break;
        passes++;
        fed += r;
        tl_stats_t st;
        tl_stats(&st);
        ps_fix_r += st.ps_rows_reread; ps_bad += st.ps_rows_bad;
        total += st.t_total; t_sd += st.t_sd; t_ps += st.t_ps; t_cmp += st.t_compute; t_hid += st.t_hidden;
        sd_total += st.sd_bytes;
        const float tc = tempmonGetTemp();
        if (tc > tmax) tmax = tc;
        char what[40];
        snprintf(what, sizeof what, "multi pass=%d slots=%d", passes, r);
        pass_lines(what, st, tc);
    }
    if (r < 0) {
        if (g_abort) { say("  STOPPED at pass %d by ::stop, between two card reads", passes + 1); say("RUN error=stopped pass=%d", passes + 1); g_abort = false; return true; }
        say("  multi pass %d failed: %s", passes + 1, tl_last_error());
        if (tl_fault_bank() >= 0) return false;
        say("RUN error=multi pass=%d", passes + 1);
        return true;
    }
    int solo = 0;
    DMAMEM static char gen[4096], ea[8192];   /* OCRAM: RAM1 is the stack's */
    for (int i = 0; i < ns; i++) {
        solo += (mseq[i].n_prompt + tl_batch() - 1) / tl_batch() + (mseq[i].fed - mseq[i].n_prompt);
        int gl = 0;
        for (int j = mseq[i].n_prompt; j <= mseq[i].fed; j++)
            gl += tl_decode(mseq[i].ids[j], gen + gl, (int)sizeof gen - gl - 1);
        esc(gen, gl, ea, (int)sizeof ea);
        say("  ANSWER S%d: \"%s\"", i, ea);
    }
    say("  %d prompts, %d passes over the weights for %d positions in %.0f s, %.1f s per pass; one at a time they "
        "would take %d passes", ns, passes, fed, total, total / (passes ? passes : 1), solo);
    say("RUN multi=%d n_gen=%d passes=%d positions=%d shared=%d solo_passes=%d total_s=%.1f per_pass_s=%.2f sd_gb=%.3f"
        " sd_mbs=%.2f sd_s=%.1f compute_s=%.1f hidden_s=%.1f ps_s=%.1f ps_fix_r=%lu ps_fix_w=%lu ps_bad=%lu ps_sw=%lu ps_sw_s=%.2f temp_max_c=%.1f",
        ns, g_gen, passes, fed, shared, solo, total, total / (passes ? passes : 1), sd_total / 1073741824.0,
        sd_total / 1048576.0 / (t_sd > 0 ? t_sd : 1), t_sd, t_cmp, t_hid, t_ps,
        (unsigned long)ps_fix_r, (unsigned long)ps_fix_w, (unsigned long)ps_bad,
        (unsigned long)(ps_switches - sw0), (ps_switch_us - swus0) / 1e6, tmax);
    say("  DONE. Type another prompt and press Enter.");
    return true;
}

static void run_multi(void)
{
    const int ns = g_multi_n;
    g_multi_n = 0;
    for (int i = 0; i < MULTI_MAX; i++) { g_multi_replay[i] = 0; g_multi_printed[i] = 0; }
    if (run_multi_once(ns)) return;
    const int fb = tl_fault_bank();
    if (fb < 0) { say("RUN error=failed"); return; }
    if (tl_bank_fault(fb)) {
        say("  chip %s (bank %d) failed and cannot be replaced: %s", use[fb].name, fb, tl_last_error());
        say("RUN error=bank_fault bank=%d", fb);
        return;
    }
    say("  chip %s (bank %d) failed: retired; its layers are now on other chips; re-running all %d prompts from position 0",
        use[fb].name, fb, ns);
    say("FAULT bank=%d chip=%s", fb, use[fb].name);
    for (int i = 0; i < ns; i++) { g_multi_replay[i] = g_multi_printed[i]; g_multi_printed[i] = 0; mseq[i].fed = 0; mseq[i].done = 0; }
    if (!run_multi_once(ns)) { say("  a second chip failed: %s", tl_last_error()); say("RUN error=bank_fault_again"); }
}

/* The M7 on real rows already in RAM: what the arithmetic costs once the card is out of the way. */
FLASHMEM static void bench_kernel(const char *label, int layer, int which)
{
    uint64_t off; uint32_t type, rows, cols, rb;
    if (tl_matrix(layer, which, &off, &type, &rows, &cols, &rb)) { say("BENCH kernel %s absent", label); return; }
    uint32_t nr = SDBUF_BYTES / rb;
    if (nr > rows) nr = rows;
    if (!nr || cols > 11008) { say("BENCH kernel %s does not fit", label); return; }
    /* sdbuf is the core's arena, in CACHED OCRAM, and SdFat's DMA does no cache maintenance: clean and drop the
     * lines first, so no stale line is read and no dirty one is written back over the card's bytes. (The
     * partial sectors SdFat copies by CPU end on 32-byte lines here: tensor offsets are 32-aligned.) */
    arm_dcache_flush_delete(sdbuf, nr * rb);
    if (plat_sd_read(off, sdbuf, nr * rb)) { say("BENCH kernel %s read failed", label); return; }
    uint32_t x = 0x2545F491u;
    for (uint32_t i = 0; i < cols; i++) { x = x * 1664525u + 1013904223u; bx[i] = ((int32_t)(x >> 8) - 8388608) / 8388608.0f; }
    gguf_quantize_act(bx, cols, bxq, bxs);
    gguf_act_sums(bxq, cols, bxsum);
    for (int path = 0; path < (type == GGML_Q4_K ? 2 : 1); path++) {
        uint32_t reps = 0;
        volatile float sink = 0;
        const uint32_t t0 = micros();
        do {
            for (uint32_t r = 0; r < nr; r++) {
                const uint8_t *p = sdbuf + (size_t)r * rb;
                sink += (type == GGML_Q4_K && path == 0) ? gguf_dot_q4k_presum(p, bxq, bxs, bxsum, cols)
                                                         : gguf_dot_q(type, p, bxq, bxs, cols);
            }
            reps++;
        } while (micros() - t0 < 1000000u);
        const float s = (micros() - t0) / 1e6f;
        say("BENCH kernel %s type %lu path %s rows %lu cols %lu mb_s %.2f mweights_s %.2f", label, (unsigned long)type,
            (type == GGML_Q4_K && path == 0) ? "presum" : "dot_q", (unsigned long)nr, (unsigned long)cols,
            (double)nr * rb * reps / 1048576.0 / s, (double)nr * cols * reps / 1e6 / s);
    }
    /* The one-token kernels as the core runs them since v9f: the activation vector's lane pairs made once
     * (gguf_widen_act) and loaded, instead of widened per weight word of every row. */
    if (type == GGML_Q4_K || type == GGML_Q6_K) {
        uint32_t *xw = bxw;
        gguf_widen_act(bxq, cols, xw);
        uint32_t reps = 0;
        volatile float sink = 0;
        const uint32_t t0 = micros();
        do {
            for (uint32_t r = 0; r < nr; r++) {
                const uint8_t *p = sdbuf + (size_t)r * rb;
                sink += type == GGML_Q4_K ? gguf_dot_q4k_presum_w(p, bxq, xw, bxs, bxsum, cols)
                                          : gguf_dot_q6k_w(p, bxq, xw, bxs, cols);
            }
            reps++;
        } while (micros() - t0 < 1000000u);
        const float s = (micros() - t0) / 1e6f;
        say("BENCH kernel %s type %lu path widened rows %lu cols %lu mb_s %.2f mweights_s %.2f", label, (unsigned long)type,
            (unsigned long)nr, (unsigned long)cols, (double)nr * rb * reps / 1048576.0 / s, (double)nr * cols * reps / 1e6 / s);
    }
    /* The batched kernel: the same rows against 8 activation vectors in one call (the same vector 8 times --
     * the arithmetic does not depend on the values). Reported per vector, to set against the lines above. */
    if (type == GGML_Q4_K || type == GGML_Q6_K) {
        const int8_t *qp[8];
        const float *sp[8];
        const int32_t *mp[8];
        for (int p = 0; p < 8; p++) { qp[p] = bxq; sp[p] = bxs; mp[p] = bxsum; }
        float o8[8];
        uint32_t reps = 0;
        volatile float sink = 0;
        const uint32_t t0 = micros();
        do {
            for (uint32_t r = 0; r < nr; r++) {
                const uint8_t *p = sdbuf + (size_t)r * rb;
                if (type == GGML_Q4_K) gguf_dot_q4k_presum_n(p, 8, qp, sp, mp, cols, o8);
                else                   gguf_dot_q_n(type, p, 8, qp, sp, cols, o8);
                sink += o8[7];
            }
            reps++;
        } while (micros() - t0 < 1000000u);
        const float s = (micros() - t0) / 1e6f;
        say("BENCH kernel_x8 %s type %lu vectors 8 mweights_s_per_vector %.2f", label, (unsigned long)type,
            8.0 * (double)nr * cols * reps / 1e6 / s);
    }
    /* Where a Q4_K dot's time goes (gguf_dot_q4k_stage): 0 the whole kernel, 1 without the float combination,
     * 2 without the scale unpack as well. Differences between adjacent stages attribute the cost. */
    if (type == GGML_Q4_K)
        for (int stage = 0; stage < 3; stage++) {
            uint32_t reps = 0;
            volatile float sink = 0;
            const uint32_t t0 = micros();
            do {
                for (uint32_t r = 0; r < nr; r++) sink += gguf_dot_q4k_stage(sdbuf + (size_t)r * rb, bxq, bxs, cols, stage);
                reps++;
            } while (micros() - t0 < 1000000u);
            const float s = (micros() - t0) / 1e6f;
            say("BENCH q4k_stage %s stage %d cycles_per_weight %.3f mweights_s %.2f", label, stage,
                (double)F_CPU_ACTUAL * s / ((double)nr * cols * reps), (double)nr * cols * reps / 1e6 / s);
        }
}

/* the card at three read sizes into the cached arena, and at 4 KB into DTCM, in the mode it is in */
/* 2 MB of the model at off in 64 KB reads: MB/s over the reads alone, and FNV-1a 64 of the bytes. The
 * target is the cached arena, so in DMA mode its lines are dropped before each read and after it (aligned
 * 64 KB reads are pure DMA, nothing copied by the CPU): the sum is of what the card delivered. */
FLASHMEM static bool sd_region(uint64_t off, uint64_t *sum, float *mbs)
{
    const uint32_t N = 2u << 20, K = 64u * 1024u;
    uint64_t h = 1469598103934665603ull;
    uint32_t us = 0;
    for (uint32_t got = 0; got < N; got += K, off += K) {
        arm_dcache_flush_delete(sdbuf, K);
        const uint32_t t0 = micros();
        if (mf_read_at(off, sdbuf, K, K)) return false;
        us += micros() - t0;
        if (g_dma) arm_dcache_delete(sdbuf, K);
        for (uint32_t i = 0; i < K; i++) { h ^= sdbuf[i]; h *= 1099511628211ull; }
    }
    *sum = h;
    *mbs = 2.0f * 1.048576f / (us / 1e6f);
    return true;
}

/* ::sdsweep -- see "THE DMA PATH'S BURSTS". FIFO first, for the reference sum; then every DMA setting. A
 * setting whose read fails restarts the card before the next. The fastest setting that read every byte
 * right is kept (g_sdcfg) and the card is left in the mode it was in. */
FLASHMEM static void sd_sweep(void)
{
    say("SWEEP begin temp_c %.1f", tempmonGetTemp());
    const bool was_dma = g_dma;
    uint64_t ref = 0, s = 0;
    float rf = 0, r = 0;
    if (g_dma && !sdio_switch(false)) { say("SWEEP end: FIFO restart failed"); return; }
    if (!sd_region(512ull << 20, &ref, &rf)) { say("SWEEP end: FIFO read failed"); return; }
    say("SWEEP fifo mb_s %.2f sum %08lx%08lx", rf, (unsigned long)(ref >> 32), (unsigned long)ref);
    g_sdcfg = false;
    if (!sdio_switch(true)) { say("SWEEP end: DMA restart failed"); return; }
    sd_regs("DMA as SdFat sets it");
    const uint32_t w0 = USDHC1_WTMK_LVL, p0 = USDHC1_PROT_CTRL;
    bool ok = sd_region(512ull << 20, &s, &r);
    say("SWEEP dma sdfat wml %lu brst %lu blen %lu mb_s %.2f data %s", (unsigned long)(w0 & 0xFF),
        (unsigned long)((w0 >> 8) & 0x1F), (unsigned long)((p0 >> 12) & 7), r, !ok ? "READ-FAILED" : s == ref ? "OK" : "WRONG");
    float best = (ok && s == ref) ? r : 0;
    uint8_t bw = 0, bb = 0, bl = 0;
    static const uint8_t WML[4] = { 16, 32, 64, 128 };
    static const uint8_t BRS[3] = { 4, 8, 16 };
    static const uint8_t BLE[3] = { 1, 3, 7 };
    for (int a = 0; a < 4; a++)
        for (int b = 0; b < 3; b++)
            for (int c = 0; c < 3; c++) {
                if (BRS[b] > WML[a]) continue;
                USDHC1_WTMK_LVL = w0; USDHC1_PROT_CTRL = p0;
                sd_burst_apply(WML[a], BRS[b], BLE[c]);
                ok = sd_region(512ull << 20, &s, &r);
                const bool good = ok && s == ref;
                say("SWEEP dma wml %u brst %u blen %u mb_s %.2f data %s", WML[a], BRS[b], BLE[c], ok ? r : 0.0f,
                    !ok ? "READ-FAILED" : good ? "OK" : "WRONG");
                if (!ok) {
                    say("SWEEP restarting the card after a failed read");
                    if (!sdio_switch(true)) { say("SWEEP end: DMA restart failed"); return; }
                }
                if (good && r > best) { best = r; bw = WML[a]; bb = BRS[b]; bl = BLE[c]; }
            }
    USDHC1_WTMK_LVL = w0; USDHC1_PROT_CTRL = p0;
    if (bw) {
        g_rwml = bw; g_rbrst = bb; g_blen = bl; g_sdcfg = true;
        sd_burst_apply(bw, bb, bl);
        say("SWEEP best wml %u brst %u blen %u mb_s %.2f (kept for DMA)", bw, bb, bl, best);
    } else {
        say("SWEEP best is SdFat's own setting, mb_s %.2f", best);
    }
    if (!was_dma && !sdio_switch(false)) { say("SWEEP end: FIFO restart failed"); return; }
    say("SWEEP end temp_c %.1f", tempmonGetTemp());
}

/* ::sdclksweep -- see "THE CARD'S CLOCK". FIFO, 8 MB in four places at SdFat's 49.5 MHz for the reference
 * sums; then each faster divider reads the same 8 MB twice. The first divider that fails or reads one wrong
 * byte ends the sweep, and the card is restarted at SdFat's clock before anything else. */
FLASHMEM static void sd_clk_sweep(void)
{
    static const uint64_t OFF[4] = { 64ull << 20, 512ull << 20, 1024ull << 20, 1536ull << 20 };
    say("CLKSWEEP begin temp_c %.1f", tempmonGetTemp());
    const bool was_dma = g_dma;
    g_sddvs = 0;
    if (!sdio_switch(false)) { say("CLKSWEEP end: FIFO restart failed"); return; }
    uint64_t ref[4];
    float r = 0, sum = 0;
    for (int i = 0; i < 4; i++) {
        if (!sd_region(OFF[i], &ref[i], &r)) { say("CLKSWEEP end: reference read failed"); return; }
        sum += r;
    }
    const uint8_t dvs0 = (uint8_t)((USDHC1_SYS_CTRL >> 4) & 0xF);
    say("CLKSWEEP khz %lu divider %u mb_s %.2f (SdFat's, the reference)", sd_clk_now(), dvs0 + 1u, sum / 4);
    /* kept: the divider with the best MEASURED rate, and only if it beats SdFat's clock by 2% -- a faster clock
     * that the card cannot feed buys nothing (this card: 22.9 MB/s at 49.5 MHz, 24.0 at 66, 23.9 at 99) */
    uint8_t best = 0;
    float best_mbs = sum / 4 * 1.02f;
    for (int dvs = (int)dvs0 - 1; dvs >= 1; dvs--) {
        sd_idle();
        if (!sd_set_dvs((uint8_t)dvs)) { say("CLKSWEEP divider %d: clock did not settle", dvs + 1); break; }
        int wrong = 0, failed = 0, n = 0;
        sum = 0;
        for (int pass = 0; pass < 2 && !failed; pass++)
            for (int i = 0; i < 4 && !failed; i++) {
                uint64_t h;
                if (!sd_region(OFF[i], &h, &r)) { failed = 1; break; }
                wrong += h != ref[i];
                sum += r; n++;
            }
        say("CLKSWEEP khz %lu divider %d mb_s %.2f regions %d wrong %d failed %d", sd_clk_now(), dvs + 1,
            n ? sum / n : 0.0f, n, wrong, failed);
        if (failed || wrong) break;
        if (sum / n > best_mbs) { best = (uint8_t)dvs; best_mbs = sum / n; }
    }
    /* back to SdFat's clock through a full restart (a failed read may have left the card mid-command) */
    if (!sdio_switch(was_dma)) { say("CLKSWEEP end: restart failed"); halt("the card did not restart; power-cycle the Teensy (unplug USB for 2 s)"); return; }
    if (best) {
        g_sddvs = best;
        sd_idle();
        sd_set_dvs(best);
        say("CLKSWEEP best khz %lu divider %u mb_s %.2f (kept)", sd_clk_now(), best + 1u, best_mbs);
    } else {
        say("CLKSWEEP best is SdFat's clock");
    }
    say("CLKSWEEP end temp_c %.1f", tempmonGetTemp());
}

/* THE CARD BY ADMA2 (v9b experiment). SdFat's DMA path uses the uSDHC's simple DMA (PROT_CTRL DMASEL 00) and
 * measured 7.3 MB/s on this card whatever the watermark and burst registers said (::sdsweep), against 22.9
 * by FIFO. NXP's own driver runs the same controller in ADMA2 mode (DMASEL 10): the transfer follows a
 * descriptor table -- {attribute, address} pairs, attribute = length << 16 | Tran (0x21) | End (0x02) --
 * instead of DS_ADDR. ::sdadma arms such a table for SdFat's own readSectors() (its DS_ADDR write is then
 * ignored) and reads 2 MB of the model at four read sizes, each summed and compared with the FIFO read.
 * A fast, right ADMA2 read is what the v7 overlap (arithmetic during the card wait) has been waiting for. */
__attribute__((aligned(32))) static uint32_t adma_desc[2 * 8];   /* up to 8 x 32 KB */
static uint32_t g_adma_chunk = 32768u;       /* bytes per descriptor (<= 65024, a multiple of 512) */
static void adma_arm(void *buf, uint32_t n)
{
    uint8_t *p = (uint8_t *)buf;
    uint32_t i = 0;
    while (n && i < 8) {
        const uint32_t k = n < g_adma_chunk ? n : g_adma_chunk;
        adma_desc[2 * i]     = (k << 16) | 0x21u | (n == k ? 0x02u : 0u);
        adma_desc[2 * i + 1] = (uint32_t)p;
        p += k; n -= k; i++;
    }
    USDHC1_ADMA_SYS_ADDR = (uint32_t)adma_desc;
    USDHC1_PROT_CTRL = (USDHC1_PROT_CTRL & ~(3u << 8)) | (2u << 8);
}
static void adma_disarm(void) { USDHC1_PROT_CTRL &= ~(3u << 8); }

FLASHMEM static bool sd_region_adma(uint64_t off, uint32_t K, uint64_t *sum, float *mbs, uint32_t *errstat)
{
    const uint32_t N = 2u << 20;
    uint64_t h = 1469598103934665603ull;
    uint32_t us = 0, bytes = 0;
    *errstat = 0;
    uint32_t arena = 0;
    tl_scratch(&arena);                                         /* sdbuf is the arena's start; the whole arena is free here */
    if (K > arena || K % 512u || K > 8u * g_adma_chunk) { say("    ADMA2 read size %lu does not fit (arena %lu, 8 descriptors of %lu)", (unsigned long)K, (unsigned long)arena, (unsigned long)g_adma_chunk); return false; }
    for (uint32_t got = 0; got < N; got += K) {
        arm_dcache_flush_delete(sdbuf, K);
        adma_arm(sdbuf, K);
        const uint32_t t0 = micros();
        const bool ok = sd.card()->readSectors((uint32_t)(mf_sector0 + (off + got) / 512u), sdbuf, K / 512u);
        us += micros() - t0;
        *errstat |= USDHC1_ADMA_ERR_STATUS;
        adma_disarm();
        if (!ok) {
            say("    ADMA2 read of %lu bytes failed: SdFat error 0x%02X data 0x%08lX, ADMA_ERR_STATUS 0x%08lX", (unsigned long)K,
                (unsigned)sd.sdErrorCode(), (unsigned long)sd.sdErrorData(), (unsigned long)*errstat);
            return false;
        }
        arm_dcache_delete(sdbuf, K);
        /* the sum covers the 2 MB region exactly, whatever the read size: a size that does not divide it reads a
         * little past the end (the 07:5x sweep summed that too and called 65,024- and 114,688-byte reads WRONG) */
        const uint32_t take = (N - got) < K ? (N - got) : K;
        for (uint32_t i = 0; i < take; i++) { h ^= sdbuf[i]; h *= 1099511628211ull; }
        bytes += K;
    }
    *sum = h;
    *mbs = (float)bytes / 1048576.0f / (us / 1e6f);
    return true;
}

FLASHMEM static void sd_adma_sweep(void)
{
    say("ADMA begin temp_c %.1f", tempmonGetTemp());
    if (!mf_contig) { say("ADMA end: the model file is not contiguous, no sector arithmetic"); return; }
    const bool was_dma = g_dma;
    uint64_t ref = 0, s = 0;
    float r = 0;
    if (g_dma && !sdio_switch(false)) { say("ADMA end: FIFO restart failed"); return; }
    if (!sd_region(512ull << 20, &ref, &r)) { say("ADMA end: FIFO read failed"); return; }
    say("ADMA fifo mb_s %.2f", r);
    if (!sdio_switch(true)) { say("ADMA end: DMA restart failed"); return; }
    if (sd_region(512ull << 20, &s, &r)) say("ADMA sdma read_size 65536 mb_s %.2f data %s", r, s == ref ? "OK" : "WRONG");
    /* read size x descriptor size (the arena, 119 KB, is the buffer: the 04:52 build read 256 KB into it and the
     * board reset itself), then the watermark and burst registers under ADMA2 at 64 KB */
    static const uint32_t SZ[7][2] = { { 4096, 32768 }, { 16384, 32768 }, { 65024, 65024 }, { 65536, 32768 }, { 65536, 16384 },
                                       { 114688, 57344 }, { 114688, 28672 } };
    for (int k = 0; k < 7; k++) {
        uint32_t es = 0;
        g_adma_chunk = SZ[k][1];
        if (!sd_region_adma(512ull << 20, SZ[k][0], &s, &r, &es)) {
            say("ADMA adma2 read_size %lu desc %lu FAILED; restarting the card", (unsigned long)SZ[k][0], (unsigned long)SZ[k][1]);
            if (!sdio_switch(true)) { say("ADMA end: DMA restart failed"); halt("the card did not restart; power-cycle the Teensy (unplug USB for 2 s)"); return; }
            continue;
        }
        say("ADMA adma2 read_size %lu desc %lu mb_s %.2f data %s adma_err 0x%08lX", (unsigned long)SZ[k][0], (unsigned long)SZ[k][1], r,
            s == ref ? "OK" : "WRONG", (unsigned long)es);
    }
    g_adma_chunk = 32768u;
    {
        const uint32_t w0 = USDHC1_WTMK_LVL, p0 = USDHC1_PROT_CTRL;
        static const uint8_t WB[4][3] = { { 128, 16, 7 }, { 64, 16, 7 }, { 16, 16, 7 }, { 128, 8, 0 } };
        for (int k = 0; k < 4; k++) {
            uint32_t es = 0;
            sd_burst_apply(WB[k][0], WB[k][1], WB[k][2]);
            const bool ok = sd_region_adma(512ull << 20, 65536, &s, &r, &es);
            USDHC1_WTMK_LVL = w0; USDHC1_PROT_CTRL = p0;
            if (!ok) { say("ADMA adma2 wml %u brst %u blen %u FAILED", WB[k][0], WB[k][1], WB[k][2]); if (!sdio_switch(true)) { halt("the card did not restart; power-cycle the Teensy (unplug USB for 2 s)"); return; } continue; }
            say("ADMA adma2 read_size 65536 wml %u brst %u blen %u mb_s %.2f data %s", WB[k][0], WB[k][1], WB[k][2], r, s == ref ? "OK" : "WRONG");
        }
    }
    sd_idle();
    if (!was_dma && !sdio_switch(false)) { say("ADMA end: FIFO restart failed"); return; }
    say("ADMA end temp_c %.1f", tempmonGetTemp());
}

FLASHMEM static void card_bench(void)
{
    static const uint32_t SZ[3] = { 4096, 16384, 65536 };
    for (int k = 0; k < 3; k++) {
        const uint32_t N = 16u << 20;
        uint64_t off = 512ull << 20;
        bool ok = true;
        const uint32_t t0 = micros();
        for (uint32_t got = 0; got < N; got += SZ[k], off += SZ[k])
            if (plat_sd_read(off, sdbuf, SZ[k])) { ok = false; break; }
        const float s = (micros() - t0) / 1e6f;
        say("BENCH sd_seq read_size %lu mb_s %.2f sdio %s%s", (unsigned long)SZ[k], 16.0 * 1.048576 / s,
            g_dma ? "dma" : "fifo", ok ? "" : " FAILED");
    }
    {   /* The same reads into DTCM (uncached, where the model's pipeline buffers are) instead of the cached
         * OCRAM arena above. v7's first boot: DMA_SDIO gave 7.26 MB/s into OCRAM at 4, 16 and 64 KB alike. */
        uint8_t dt[4096] __attribute__((aligned(32)));      /* on the stack: DTCM too, and no RAM1 kept for it */
        const uint32_t N = 16u << 20;
        uint64_t off = 512ull << 20;
        bool ok = true;
        const uint32_t t0 = micros();
        for (uint32_t got = 0; got < N; got += sizeof dt, off += sizeof dt)
            if (plat_sd_read(off, dt, sizeof dt)) { ok = false; break; }
        const float s = (micros() - t0) / 1e6f;
        say("BENCH sd_seq_dtcm read_size %lu mb_s %.2f sdio %s clock_khz %lu%s", (unsigned long)sizeof dt,
            16.0 * 1.048576 / s, g_dma ? "dma" : "fifo", sd_clk_now(), ok ? "" : " FAILED");
    }
}

FLASHMEM static void bench(void)
{
    say("BENCH begin temp_c %.1f", tempmonGetTemp());
    /* both card modes on the same boot, then back to the one it was in (every command leaves the card idle) */
    const bool was = g_dma;
    card_bench();
    if (sdio_switch(!was)) card_bench();
    else say("BENCH sdio switch to %s failed", was ? "fifo" : "dma");
    if (!sdio_switch(was)) { halt("the card did not restart; power-cycle the Teensy (unplug USB for 2 s)"); return; }
    {
        uint32_t x = 12345u;
        int fails = 0;
        const uint32_t blocks = (uint32_t)(mf_size / 4096u) - 1u;
        const uint32_t t0 = micros();
        for (int i = 0; i < 256; i++) {
            x = x * 1664525u + 1013904223u;
            if (plat_sd_read((uint64_t)(x % blocks) * 4096u, sdbuf, 4096)) fails++;
        }
        const float s = (micros() - t0) / 1e6f;
        say("BENCH sd_random read_size 4096 count 256 ms_each %.3f mb_s %.2f fails %d", s * 1000.0f / 256.0f,
            256.0 * 4096.0 / 1048576.0 / s, fails);
    }
    bench_kernel("ffn_gate_L0", 0, 4);
    bench_kernel("ffn_down_L0", 0, 6);
    bench_kernel("attn_v_L0", 0, 2);
    bench_kernel("token_embd", 0, 7);
    /* PSRAM: a bank holding no layer if there is one (a spare), else layer 0's bank -- between prompts the
     * cache is rewritten by the next prompt anyway. One megabyte: raw, then through the platform (a verified
     * write, a single read), then the cache the way attention uses it (tl_cache_bench, layer 0). */
    {
        int bi = -1;
        for (int b = 0; b < nuse && bi < 0; b++) {
            bool held = false;
            for (int l = 0; l < info.n_layer && !held; l++) held = tl_layer_bank(l) == b;
            if (!held) bi = b;
        }
        if (bi < 0) bi = tl_layer_bank(0);
        const uint32_t N = 1u << 20;
        ps_bank(bi);
        g_tag = 0x77;
        uint32_t t0 = micros();
        fill(0, N);
        const float sw = (micros() - t0) / 1e6f;
        t0 = micros();
        const uint32_t bad = verify(0, N);
        const float sr = (micros() - t0) / 1e6f;
        say("BENCH psram_raw bank %s mode %s write_mb_s %.2f read_mb_s %.2f wrong %lu", use[bi].name,
            MODE_NAME[use[bi].mode], 1.048576 / sw, 1.048576 / sr, (unsigned long)bad);
        for (uint32_t i = 0; i < BLK; i++) buf[i] = pat(i);
        t0 = micros();
        int ew = 0;
        for (uint32_t o = 0; o < N; o += BLK) ew |= plat_ps_write(bi, o, buf, BLK);
        const float cw = (micros() - t0) / 1e6f;
        t0 = micros();
        int er = 0;
        for (uint32_t o = 0; o < N; o += BLK) er |= plat_ps_read(bi, o, sdbuf, BLK);
        const float cr = (micros() - t0) / 1e6f;
        say("BENCH psram_plat bank %s verified_write_mb_s %.2f read_mb_s %.2f errors %d", use[bi].name,
            1.048576 / cw, 1.048576 / cr, ew | er);
        double tw = 0, tr = 0;
        uint32_t rr = 0;
        const int np = info.max_seq < 1024 ? info.max_seq : 1024;
        const uint32_t sw0 = ps_switches;
        if (tl_cache_bench(np, &tw, &tr, &rr))
            say("BENCH cache_layer failed: %s", tl_last_error());
        else
            say("BENCH cache_layer bank %s positions %d write_s %.3f read_s %.3f ms_per_position_read %.3f rows_reread %lu bank_switches %lu",
                use[tl_layer_bank(0)].name, np, tw, tr, tr * 1000.0 / np, (unsigned long)rr, (unsigned long)(ps_switches - sw0));
    }
    say("BENCH end temp_c %.1f", tempmonGetTemp());
}

FLASHMEM static void stats(void)
{
    for (int i = 0; i < nuse; i++)
        say("STATS bank %d %s %s write %d read %d burst %d rate_mb_s %.2f", i, use[i].name, MODE_NAME[use[i].mode],
            NOPS[use[i].wi], NOPS[use[i].ri], use[i].nb, use[i].rate);
    say("STATS ps_fix_r %lu ps_fix_w %lu ps_bad %lu temp_c %.1f gen %d max_seq %d uptime_s %lu",
        (unsigned long)ps_fix_r, (unsigned long)ps_fix_w, (unsigned long)ps_bad, tempmonGetTemp(), g_gen,
        info.max_seq, (unsigned long)(millis() / 1000));
}

static void dispatch_one(char *line);
static void dispatch(char *line) { dispatch_one(line); sd_idle(); }   /* every command ends with the card idle */
FLASHMEM static void dispatch_one(char *line)
{
    if (!strcmp(line, "I") || !strcmp(line, "i")) {
        say("I bench-one psram_llm v9 banks %d ps_bytes %lu max_seq %d gen %d sdio %s overlap %d align %d slice %lu clk %lu model %s", nuse,
            (unsigned long)nuse * BANKSZ, info.max_seq, g_gen, g_dma ? "dma" : "fifo", g_overlap, g_align, (unsigned long)g_slice_us,
            sd_clk_now(), g_model_name);
        return;
    }
    if (!strncmp(line, "::model", 7)) {                      /* ::model NAME.gguf: record it, restart, open it */
        const char *a = line + 7;
        while (*a == ' ') a++;
        if (!*a) { say("MODEL %s (tokenizer store %s)", g_model_name, g_tok_name); return; }
        const size_t L = strlen(a);
        if (L < 6 || L > 40 || strcmp(a + L - 5, ".gguf") || !sd.exists(a)) { say("MODEL %s: not a .gguf on the card", a); return; }
        FsFile f = sd.open("model.txt", O_RDWR | O_CREAT | O_TRUNC);
        if (!f) { say("MODEL: cannot write model.txt"); return; }
        f.write(a, L); f.write("\n", 1); f.close();
        say("MODEL %s saved to model.txt; restarting the board to open it (about a minute)", a);
        sd_idle();                                          /* the card is idle: a restart here cannot hang it */
        Serial.flush();
        delay(300);
        SCB_AIRCR = 0x05FA0004;                              /* Teensy 4 system reset */
        for (;;) {}
    }
    if (!strncmp(line, "::gen", 5)) {
        int v = atoi(line + 5);
        if (v < 1) v = 1;
        if (v > MAX_GEN) v = MAX_GEN;
        g_gen = v;
        say("GEN %d", g_gen);
        return;
    }
    if (!strcmp(line, "::bench")) { bench(); return; }
    if (!strcmp(line, "::stats")) { stats(); return; }
    if (!strcmp(line, "::hold"))  { say("HOLD waiting for prompts"); return; }
    if (!strncmp(line, "::prefill", 9)) { g_prefill = atoi(line + 9) ? 1 : 0; say("PREFILL %d", g_prefill); return; }
    if (!strcmp(line, "::stop"))  { g_abort = false; say("STOPPED (was idle)"); return; }
    if (!strncmp(line, "::sdio", 6)) {
        const bool dma = strstr(line + 6, "dma") != NULL;
        if (sdio_switch(dma)) say("SDIO %s", dma ? "dma" : "fifo");
        else { say("SDIO failed"); halt("the card did not restart; power-cycle the Teensy (unplug USB for 2 s)"); }
        return;
    }
    if (!strncmp(line, "::overlap", 9)) { g_overlap = atoi(line + 9) ? 1 : 0; say("OVERLAP %d (card %s)", g_overlap, g_dma ? "DMA_SDIO" : "FIFO_SDIO: no overlap possible"); return; }
    if (!strncmp(line, "::align", 7)) { g_align = atoi(line + 7) ? 1 : 0; say("ALIGN %d (card %s)", g_align, g_dma ? "DMA_SDIO" : "FIFO_SDIO: reads stay exact"); return; }
    if (!strcmp(line, "::sdsweep")) { sd_sweep(); return; }
    if (!strcmp(line, "::sdclksweep")) { sd_clk_sweep(); return; }
    if (!strcmp(line, "::sdadma")) { sd_adma_sweep(); return; }
    if (!strncmp(line, "::admamin", 9)) {                    /* ::admamin N: passes with >= N positions go by ADMA2; 0 = never */
        long v = strtol(line + 9, NULL, 10);
        if (v < 0) v = 0;
        if (v > 8) v = 8;
        g_adma_min = (int)v;
        say("ADMAMIN %d%s", g_adma_min, g_adma_min ? "" : " (the card path is only what ::sdio/::sdpath set)");
        return;
    }
    if (!strncmp(line, "::sdpath", 8)) {                     /* ::sdpath file|adma: how a pipeline read reaches the card */
        g_sdpath = strstr(line + 8, "adma") ? 1 : 0;
        say("SDPATH %s%s", g_sdpath ? "adma" : "file", g_sdpath && !g_dma ? " (takes effect in DMA mode: ::sdio dma)" : "");
        return;
    }
    if (!strncmp(line, "::sdclk", 7)) {                      /* ::sdclk N: divide 198 MHz by N (2..16); 0 = SdFat's */
        const unsigned long d = strtoul(line + 7, NULL, 10);
        if (d >= 2 && d <= 16) { g_sddvs = (uint8_t)(d - 1); sd_idle(); sd_set_dvs(g_sddvs); }
        else { g_sddvs = 0; if (!sdio_switch(g_dma)) { say("SDCLK failed"); halt("the card did not restart; power-cycle the Teensy (unplug USB for 2 s)"); return; } }
        say("SDCLK khz %lu divider %lu", sd_clk_now(), ((USDHC1_SYS_CTRL >> 4) & 0xF) + 1ul);
        return;
    }
    if (!strcmp(line, "::sdregs"))  { sd_regs(g_dma ? "DMA" : "FIFO"); say("SDREGS cfg %d wml %u brst %u blen %u", g_sdcfg, g_rwml, g_rbrst, g_blen); return; }
    if (!strncmp(line, "::sdcfg", 7)) {
        char *e1, *e2, *e3;                       /* strtoul, not sscanf: scanf costs RAM1 28 KB of tables */
        const unsigned long w = strtoul(line + 7, &e1, 10), b = strtoul(e1, &e2, 10), l = strtoul(e2, &e3, 10);
        const int n = (e1 != line + 7) + (e2 != e1) + (e3 != e2);
        if (n == 3 && w >= 1 && w <= 128 && b >= 1 && b <= 31 && b <= w && l <= 7) {
            g_rwml = (uint8_t)w; g_rbrst = (uint8_t)b; g_blen = (uint8_t)l; g_sdcfg = true;
            sd_cfg_reapply();
        } else {
            g_sdcfg = false;                          /* SdFat's own: restart the card so cardInit sets them */
            if (!sdio_switch(g_dma)) { say("SDCFG failed"); halt("the card did not restart; power-cycle the Teensy (unplug USB for 2 s)"); return; }
        }
        say("SDCFG %s wml %u brst %u blen %u", g_sdcfg ? "set" : "sdfat", g_rwml, g_rbrst, g_blen);
        return;
    }
    if (!strncmp(line, "::slice", 7)) {
        long v = atol(line + 7);
        if (v < 0) v = 0;
        if (v > 100000) v = 100000;
        g_slice_us = (uint32_t)v;
        say("SLICE %lu us of rows per yield%s", (unsigned long)g_slice_us, g_slice_us ? "" : " (0: one row per yield)");
        return;
    }
    if (!strncmp(line, "::share", 7)) { const int on = atoi(line + 7) ? 1 : 0; tl_multi_share(on); say("SHARE %d", on); return; }
    if (!strncmp(line, "::multi", 7)) {
        int v = atoi(line + 7);
        if (v < 0) v = 0;
        if (v > MULTI_MAX) v = MULTI_MAX;
        g_multi_want = v;
        g_multi_n = 0;
        say("MULTI %d", v);
        return;
    }
    unescape(line);
    if (g_multi_want > 0) {                   /* one of the prompts of a ::multi: encode it and wait for the rest */
        tl_seq_t *q = &mseq[g_multi_n];
        const int n = tl_encode(line, mids[g_multi_n], MULTI_IDS - g_gen - 1);
        if (n <= 0) { say("  multi prompt %d did not fit; ::multi cancelled", g_multi_n); say("RUN error=encode"); g_multi_want = 0; g_multi_n = 0; return; }
        q->ids = mids[g_multi_n]; q->n_prompt = n; q->n_gen = g_gen;
        DMAMEM static char ep[4096];
        esc(line, (int)strlen(line), ep, (int)sizeof ep);
        say("  multi prompt %d: \"%s\"  (%d tokens)", g_multi_n, ep, n);
        Serial.printf("S%d prompt ids:", g_multi_n);
        for (int i = 0; i < n; i++) { Serial.print(' '); Serial.print((long)mids[g_multi_n][i]); }
        Serial.println();
        g_multi_n++;
        if (--g_multi_want == 0) run_multi();
        return;
    }
    run_prompt(line);
}

FLASHMEM void setup()
{
    /* from psram_worker setup(), verbatim: FlexSPI off and its pads handed to GPIO9 */
    FLEXSPI2_MCR0 |= FLEXSPI_MCR0_MDIS;
    IOMUXC_SW_MUX_CTL_PAD_GPIO_EMC_22 = 5; IOMUXC_SW_MUX_CTL_PAD_GPIO_EMC_24 = 5;
    IOMUXC_SW_MUX_CTL_PAD_GPIO_EMC_25 = 5; IOMUXC_SW_MUX_CTL_PAD_GPIO_EMC_26 = 5;
    IOMUXC_SW_MUX_CTL_PAD_GPIO_EMC_27 = 5; IOMUXC_SW_MUX_CTL_PAD_GPIO_EMC_28 = 5;
    IOMUXC_SW_MUX_CTL_PAD_GPIO_EMC_29 = 5;
    __asm__ volatile("dsb");

    pinMode(PIN_A, OUTPUT); pinMode(PIN_B, OUTPUT); pinMode(PIN_C, OUTPUT);
    pinMode(PIN_DEC, OUTPUT); digitalWriteFast(PIN_DEC, HIGH);
    for (int p = 48; p <= 54; p++) pinMode(p, OUTPUT);
    GPIO9_DR |= (B_SS0 | B_SS1 | B_DEC);
    bus_out();
    ARM_DEMCR |= ARM_DEMCR_TRCENA;
    ARM_DWT_CTRL |= ARM_DWT_CTRL_CYCCNTENA;

    Serial.begin(115200);
    const uint32_t w0 = millis();
    while (!Serial && millis() - w0 < 5000) delay(10);
    delay(200);
    say("\n========================================================================");
    say("  psram_llm v9 -- a 3B-parameter language model on a Teensy 4.1");
    say("  the model: all of it on the microSD, streamed every token");
    say("  PSRAM:     the attention cache -- the model's working memory, nothing of the model");
    say("  cpu %lu MHz, %.1f C", (unsigned long)(F_CPU_ACTUAL / 1000000), tempmonGetTemp());
    say("========================================================================");

    if (!scratch_init()) { halt("the core arena is smaller than the card test needs"); return; }
    psram_bring_up();
    if (nuse < 2) { halt("fewer than two PSRAM banks passed; building the tokenizer's card file needs 8.3 MB of scratch"); return; }

    say("\n  SD card");
    /* RETRIED, AND EVERY FAILURE NAMED. The 2026-09-27 03:26 boot halted here after a flash that rebooted the
     * Teensy while the previous firmware was mid-way through reading the card -- a Teensy reset does not
     * power-cycle the card. SD spec: CMD0 resets a card from any state but "inactive", so a retry should
     * recover it; SdFat's error code says which step of CMD0 / CMD8 / ACMD41 / CMD2... failed if not. */
    bool sd_ok = false;
    for (int a = 1; a <= 8 && !sd_ok; a++) {
        const bool fifo = (a % 2) == 0;          /* DMA_SDIO first: it lets reads overlap the arithmetic */
        if (sd.begin(SdioConfig(fifo ? FIFO_SDIO : DMA_SDIO))) {
            say("    SD up on attempt %d (%s), clock %lu kHz", a, fifo ? "FIFO_SDIO" : "DMA_SDIO", sd_khz());
            g_sd_up = true;
            g_dma = !fifo;
            sd_regs("at boot");
            sd_ok = true;
        } else {
            say("    SD begin attempt %d (%s) failed: SdFat error 0x%02X data 0x%08lX", a, fifo ? "FIFO_SDIO" : "DMA_SDIO",
                (unsigned)sd.sdErrorCode(), (unsigned long)sd.sdErrorData());
            delay(250u * (uint32_t)a);
        }
    }
    if (!sd_ok) { halt("the SD card would not initialise; power-cycle the Teensy (unplug USB for 2 s)"); return; }
    model_name_load();                            /* model.txt, if ::model chose another file */
    if (!model_file_open()) {
        static char hm[80];
        snprintf(hm, sizeof hm, "%s not found on the card", g_model_name);
        halt(hm); return;
    }
    sd_clk_sweep();                               /* the fastest card clock this card reads right at, checked */
    {
        const uint32_t N = 32u * 1024u * 1024u;
        const uint32_t t0 = micros();
        uint32_t got = 0;
        bool ok = true;
        for (uint64_t off = 64ull << 20; got < N; off += SDBUF_BYTES, got += SDBUF_BYTES)
            if (plat_sd_read(off, sdbuf, SDBUF_BYTES)) { ok = false; break; }
        const float s = (micros() - t0) / 1e6f;
        if (!ok) { halt("SD read failed during the rate test"); return; }
        say("    sequential read: 32 MB in %.2f s = %.2f MB/s", s, 32.0f * 1.048576f / s);
    }

    say("\n  opening the model: all of it stays on the card; the PSRAM gets the attention cache and nothing else");
    char err[200];
    const uint32_t t0 = millis();
    const int opened = tl_open(&info, err, sizeof err);
    say("    %.1f s; PSRAM self-check: %lu reads corrected, %lu writes re-done, %lu unresolved",
        (millis() - t0) / 1000.0, (unsigned long)ps_fix_r, (unsigned long)ps_fix_w, (unsigned long)ps_bad);
    if (opened) { halt(err); return; }
    say("    %d layers, dim %d, hidden %d, %d heads (%d kv), head_dim %d, vocabulary %d, %lu merges",
        info.n_layer, info.dim, info.hidden, info.n_heads, info.n_kv, info.head_dim, info.vocab,
        (unsigned long)info.n_merges);
    say("    %.0f parameters in the file", info.params);
    say("    read from the card per token: %llu bytes", (unsigned long long)info.sd_bytes_per_token);
    say("    the model stays on the card: weights, norms and biases read from %s every pass; the tokenizer's"
        " tables in %s (%lu bytes, %s)", g_model_name, g_tok_name, (unsigned long)info.tok_store_bytes,
        info.tok_built ? "built from the model just now" : "checked against the model and reused");
    say("    PSRAM: %d chips, the attention cache on %d of them, one layer per region (%lu bytes), %d positions;"
        " %d layer slots, %d spare (%s) -- nothing of the model", info.banks, info.banks_used,
        (unsigned long)info.layer_bytes, info.max_seq, info.layer_slots, info.spare_slots,
        info.one_bank_spare ? "any one chip can fail" : "not a whole chip's worth");
    {
        char lb[200];
        int k = 0;
        for (int l = 0; l < info.n_layer && k < (int)sizeof lb - 8; l++)
            k += snprintf(lb + k, sizeof lb - (size_t)k, "%s%s", l ? " " : "", use[tl_layer_bank(l)].name);
        say("    layer -> chip: %s", lb);
    }
    ready = true;

    sd_idle();                                /* the rate test and tl_open left a transfer open */
    static char line[2048];
    say("\n  READY. Type a prompt and press Enter within 30 s, or the default runs");
    if (read_line(line, sizeof line, 30000)) dispatch(line);
    else { snprintf(line, sizeof line, "The capital of France is"); run_prompt(line); sd_idle(); }
}

void loop()
{
    if (!ready) { delay(1000); return; }
    static char line[2048];
    if (read_line(line, sizeof line, 1000)) dispatch(line);
}
