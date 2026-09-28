
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

static bool confirm(bank_t &b)
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

static bool qualify(bank_t &out)
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
static bool stable(const bank_t &b)
{
    apply(b);
    fill(0, SPAN);
    for (int k = 0; k < STABLE_PASSES; k++) {
        const uint32_t bad = verify(0, SPAN);
        if (bad) { say("      %s: %lu wrong on stability pass %d; leaving it out", b.name, (unsigned long)bad, k + 1); return false; }
    }
    return true;
}

static void psram_bring_up(void)
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
static bool g_dma;                           /* the card came up in DMA_SDIO */
static int  g_overlap = 1;                   /* ::overlap 0|1 */
static int  g_align = 1;                     /* ::align 0|1: whole sectors per pipeline read, DMA only */
static uint32_t g_slice_us;                  /* ::slice N: 0 = one row per yield() */
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

/* the model file open, and whether its clusters are one run (then every seek is one addition) */
static bool model_file_open(void)
{
    mf = sd.open("qwen3b.gguf", O_RDONLY);
    if (!mf) return false;
    mf_size = mf.fileSize();
    say("    qwen3b.gguf: %llu bytes", (unsigned long long)mf_size);
    uint32_t s0 = 0, s1 = 0;
    const uint32_t t0 = millis();
    mf_contig = mf.contiguousRange(&s0, &s1);
    const uint8_t ft = sd.fatType();
    if (mf_contig) {
        say("    contiguous on the card, sectors %lu..%lu (%s): every seek is one addition (checked in %lu ms)",
            (unsigned long)s0, (unsigned long)s1, ft == FAT_TYPE_EXFAT ? "exFAT" : "FAT32", (unsigned long)(millis() - t0));
    } else {
        for (int i = 0; i < MF_POOL; i++) { mfp[i] = sd.open("qwen3b.gguf", O_RDONLY); mfp_pos[i] = 0; mfp_used[i] = 0; }
        say("    FRAGMENTED on the card (%s): backward seeks walk the cluster chain; %d forward-moving read handles instead",
            ft == FAT_TYPE_EXFAT ? "exFAT" : "FAT32", MF_POOL);
    }
    return true;
}

/* ::sdio fifo|dma -- restart the card in the other mode, for an A/B on one build: close every handle, close
 * the transfer (sd_idle: the card is left idle, never mid-transfer), end, begin in the new mode, reopen. */
static bool sdio_switch(bool dma)
{
    sd_idle();
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
    say("    SD up (%s), clock %lu kHz", dma ? "DMA_SDIO" : "FIFO_SDIO", sd_khz());
    return model_file_open();
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
static int mf_read_at(uint64_t off, void *dst, uint32_t n, uint32_t need)
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
uint32_t plat_ps_size(void) { return (uint32_t)nuse * BANKSZ; }

static inline int ps_bank(uint32_t a)
{
    const int bi = (int)(a / BANKSZ);
    if (bi >= nuse) return -1;
    if (bi != cur_bank) { apply(use[bi]); cur_bank = bi; }
    return bi;
}
/* EVERY TRANSFER CHECKS ITSELF.
 *
 * A write is read back and compared, and re-done until it reads back right. A read is done twice; if the
 * two copies disagree a third decides each byte by majority. Every correction is counted and the first
 * few are logged with their address, so a marginal bank shows up as numbers in the log instead of as a
 * wrong word the model trips over later. It doubles PSRAM traffic, which is about 1 MB a token against
 * 1.9 GB read off the card -- invisible in the token time.
 *
 * What it cannot catch is a read that is wrong the SAME way every time; that is what MARGIN is for. */
static uint8_t  vchk[BLK] __attribute__((aligned(32)));
static uint8_t  vchk2[BLK] __attribute__((aligned(32)));
static uint32_t ps_fix_r, ps_fix_w, ps_bad, ps_events;

static void ps_event(const char *what, uint32_t a, uint32_t n, uint32_t nbytes)
{
    if (ps_events++ < 16)
        say("    PSRAM self-check: %s at %lu (+%lu), %lu byte(s) differed", what, (unsigned long)a,
            (unsigned long)n, (unsigned long)nbytes);
}

static int ps_read_chunk(uint32_t a, uint8_t *d, uint32_t k)          /* one bank, k <= BLK */
{
    const uint32_t off = a % BANKSZ;
    do_read(off, d, k);
    do_read(off, vchk, k);
    if (!memcmp(d, vchk, k)) return 0;
    do_read(off, vchk2, k);
    uint32_t differ = 0, unresolved = 0;
    for (uint32_t i = 0; i < k; i++) {
        if (d[i] == vchk[i] && d[i] == vchk2[i]) continue;
        differ++;
        if (vchk[i] == vchk2[i]) d[i] = vchk[i];
        else if (d[i] != vchk2[i]) unresolved++;
    }
    ps_fix_r++;
    ps_event(unresolved ? "read, three copies disagree" : "read corrected by majority", a, k, differ);
    if (unresolved) { ps_bad += unresolved; return -1; }
    return 0;
}

static int ps_write_chunk(uint32_t a, const uint8_t *s, uint32_t k)   /* one bank, k <= BLK */
{
    const uint32_t off = a % BANKSZ;
    for (int attempt = 0; attempt < 4; attempt++) {
        do_write(off, s, k);
        do_read(off, vchk, k);
        if (!memcmp(s, vchk, k)) {
            if (attempt) { ps_fix_w++; ps_event("write re-done", a, k, (uint32_t)attempt); }
            return 0;
        }
    }
    ps_bad++;
    ps_event("write never read back right", a, k, 4);
    return -1;
}

int plat_ps_read(uint32_t a, void *dst, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n) {
        if (ps_bank(a) < 0) return -1;
        const uint32_t off = a % BANKSZ;
        uint32_t k = (n < BANKSZ - off) ? n : (BANKSZ - off);
        if (k > BLK) k = BLK;
        if (ps_read_chunk(a, d, k)) return -1;
        a += k; d += k; n -= k;
    }
    return 0;
}
int plat_ps_write(uint32_t a, const void *src, uint32_t n)
{
    const uint8_t *s = (const uint8_t *)src;
    while (n) {
        if (ps_bank(a) < 0) return -1;
        const uint32_t off = a % BANKSZ;
        uint32_t k = (n < BANKSZ - off) ? n : (BANKSZ - off);
        if (k > BLK) k = BLK;
        if (ps_write_chunk(a, s, k)) return -1;
        a += k; s += k; n -= k;
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
 *    I              identity: "I bench-one psram_llm v7 ..." -- tools/bench_run.py asks this before flashing
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
static uint8_t *sdbuf;
static float   *bx;
static int8_t  *bxq;
static bool scratch_init(void)
{
    uint32_t n = 0;
    uint8_t *s = (uint8_t *)tl_scratch(&n);
    if (n < SDBUF_BYTES + 11008u * 4u + 11008u) return false;
    sdbuf = s;
    bx = (float *)(s + SDBUF_BYTES);
    bxq = (int8_t *)(s + SDBUF_BYTES + 11008u * 4u);
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
static void emit_step(const char *prompt, int n, int step, int32_t fed, int32_t t1, float l1, int32_t t2, float l2,
                      char *gen, int *gl, int genmax)
{
    static char txt[256], e[1024];
    const int k = tl_decode(t1, txt, (int)sizeof txt - 1);
    esc(txt, k, e, (int)sizeof e);
    char line[1400];
    snprintf(line, sizeof line, "step %d pos %d fed %ld -> top1 %ld %.9g top2 %ld %.9g text \"%s\"",
             step, step, (long)fed, (long)t1, (double)l1, (long)t2, (double)l2, e);
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
    say("    %s: %.1f s: SD %.1f MB in %.1f s (%.2f MB/s), PSRAM %.0f KB read %.1f KB written in %.2f s, compute %.1f s;"
        " PSRAM self-check so far: %lu reads corrected, %lu writes re-done, %lu unresolved", what,
        st.t_total, st.sd_bytes / 1048576.0, st.t_sd, st.sd_bytes / 1048576.0 / (st.t_sd > 0 ? st.t_sd : 1),
        st.ps_read / 1024.0, st.ps_written / 1024.0, st.t_ps, st.t_compute,
        (unsigned long)ps_fix_r, (unsigned long)ps_fix_w, (unsigned long)ps_bad);
    say("STAGES %s embed=%.3f qkv=%.2f att=%.3f wo=%.2f ffn=%.2f head=%.2f hidden=%.2f temp_c=%.1f",
        what, st.t_embed, st.t_qkv, st.t_att, st.t_wo, st.t_ffn, st.t_head, st.t_hidden, tc);
}

static void run_prompt(const char *prompt)
{
    g_abort = false;                      /* a ::stop that arrived while idle is not a stop for this run */
    static int32_t ids[1024];
    const int n = tl_encode(prompt, ids, 1024);
    if (n <= 0) { say("  prompt did not encode"); say("RUN error=encode"); return; }
    if (n + g_gen > info.max_seq) { say("  prompt too long for the %d-position cache", info.max_seq); say("RUN error=too_long"); return; }
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
            tl_stats_reset();
            if (tl_prefill(ids + p0, k, p0, a1, b1, a2, b2)) {
                if (g_abort) { say("  STOPPED in the prompt by ::stop, between two card reads"); say("RUN error=stopped step=%d", p0); g_abort = false; return; }
                say("  prefill failed at positions %d..%d: %s", p0, p0 + k - 1, tl_last_error());
                say("RUN error=prefill step=%d", p0);
                return;
            }
            passes++;
            tl_stats_t st;
            tl_stats(&st);
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
                return;
            }
            say("  forward failed at step %d: %s", step, tl_last_error());
            say("RUN error=forward step=%d", step);
            return;
        }
        passes++;
        tl_stats_t st;
        tl_stats(&st);
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
    say("  PSRAM self-check over the whole run: %lu reads corrected, %lu writes re-done, %lu unresolved",
        (unsigned long)ps_fix_r, (unsigned long)ps_fix_w, (unsigned long)ps_bad);
    say("RUN n_prompt=%d n_gen=%d prefill=%d passes=%d total_s=%.1f prompt_s=%.1f per_pass_s=%.2f sd_gb=%.3f sd_mbs=%.2f"
        " sd_s=%.1f compute_s=%.1f hidden_s=%.1f ps_s=%.1f ps_fix_r=%lu ps_fix_w=%lu ps_bad=%lu temp_max_c=%.1f",
        n, g_gen, g_prefill, passes, total, t_prompt, total / (passes ? passes : 1), sd_total / 1073741824.0,
        sd_total / 1048576.0 / (t_sd > 0 ? t_sd : 1), t_sd, t_cmp, t_hid, t_ps,
        (unsigned long)ps_fix_r, (unsigned long)ps_fix_w, (unsigned long)ps_bad, tmax);
    say("  DONE. Type another prompt and press Enter.");
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
    snprintf(line, sizeof line, "S%d step %d pos %d fed %ld -> top1 %ld %.9g top2 %ld %.9g text \"%s\"",
             s, pos, pos, (long)fed, (long)t1, (double)l1, (long)t2, (double)l2, e);
    Serial.println(line);
}

static void run_multi(void)
{
    g_abort = false;
    const int ns = g_multi_n;
    g_multi_n = 0;
    if (tl_multi_begin(mseq, ns)) { say("  multi: %s", tl_last_error()); say("RUN error=multi"); return; }
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
        tl_stats_reset();
        r = tl_multi_pass(mseq, ns, multi_emit, NULL);
        if (r <= 0) break;
        passes++;
        fed += r;
        tl_stats_t st;
        tl_stats(&st);
        total += st.t_total; t_sd += st.t_sd; t_ps += st.t_ps; t_cmp += st.t_compute; t_hid += st.t_hidden;
        sd_total += st.sd_bytes;
        const float tc = tempmonGetTemp();
        if (tc > tmax) tmax = tc;
        char what[40];
        snprintf(what, sizeof what, "multi pass=%d slots=%d", passes, r);
        pass_lines(what, st, tc);
    }
    if (r < 0) {
        if (g_abort) { say("  STOPPED at pass %d by ::stop, between two card reads", passes + 1); say("RUN error=stopped pass=%d", passes + 1); g_abort = false; return; }
        say("  multi pass %d failed: %s", passes + 1, tl_last_error());
        say("RUN error=multi pass=%d", passes + 1);
        return;
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
        " sd_mbs=%.2f sd_s=%.1f compute_s=%.1f hidden_s=%.1f ps_s=%.1f ps_fix_r=%lu ps_fix_w=%lu ps_bad=%lu temp_max_c=%.1f",
        ns, g_gen, passes, fed, shared, solo, total, total / (passes ? passes : 1), sd_total / 1073741824.0,
        sd_total / 1048576.0 / (t_sd > 0 ? t_sd : 1), t_sd, t_cmp, t_hid, t_ps,
        (unsigned long)ps_fix_r, (unsigned long)ps_fix_w, (unsigned long)ps_bad, tmax);
    say("  DONE. Type another prompt and press Enter.");
}

/* The M7 on real rows already in RAM: what the arithmetic costs once the card is out of the way. */
static void bench_kernel(const char *label, int layer, int which)
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
static void card_bench(void)
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
            16.0 * 1.048576 / s, g_dma ? "dma" : "fifo", sd_khz(), ok ? "" : " FAILED");
    }
}

static void bench(void)
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
    /* PSRAM on the first bank wholly above the model, so nothing the model holds is touched */
    const uint32_t a0 = ((info.ps_used + BANKSZ - 1) / BANKSZ) * BANKSZ;
    if (a0 + (1u << 20) <= plat_ps_size()) {
        const int bi = (int)(a0 / BANKSZ);
        const uint32_t N = 1u << 20;
        apply(use[bi]); cur_bank = bi;
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
        for (uint32_t o = 0; o < N; o += BLK) ew |= plat_ps_write(a0 + o, buf, BLK);
        const float cw = (micros() - t0) / 1e6f;
        t0 = micros();
        int er = 0;
        for (uint32_t o = 0; o < N; o += BLK) er |= plat_ps_read(a0 + o, sdbuf, BLK);
        const float cr = (micros() - t0) / 1e6f;
        say("BENCH psram_checked bank %s write_mb_s %.2f read_mb_s %.2f errors %d", use[bi].name,
            1.048576 / cw, 1.048576 / cr, ew | er);
    } else {
        say("BENCH psram no free bank above the model");
    }
    say("BENCH end temp_c %.1f", tempmonGetTemp());
}

static void stats(void)
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
static void dispatch_one(char *line)
{
    if (!strcmp(line, "I") || !strcmp(line, "i")) {
        say("I bench-one psram_llm v7 banks %d ps_bytes %lu max_seq %d gen %d sdio %s overlap %d align %d slice %lu", nuse,
            (unsigned long)plat_ps_size(), info.max_seq, g_gen, g_dma ? "dma" : "fifo", g_overlap, g_align, (unsigned long)g_slice_us);
        return;
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

void setup()
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
    say("  psram_llm v7 -- a 3B-parameter language model on a Teensy 4.1");
    say("  weights: streamed from the microSD, every token");
    say("  memory:  the PSRAM banks -- tokenizer, norms, attention cache");
    say("  cpu %lu MHz, %.1f C", (unsigned long)(F_CPU_ACTUAL / 1000000), tempmonGetTemp());
    say("========================================================================");

    if (!scratch_init()) { halt("the core arena is smaller than the card test needs"); return; }
    psram_bring_up();
    if (nuse < 2) { halt("fewer than two PSRAM banks passed; the tokenizer alone needs 8.3 MB"); return; }

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
            sd_ok = true;
        } else {
            say("    SD begin attempt %d (%s) failed: SdFat error 0x%02X data 0x%08lX", a, fifo ? "FIFO_SDIO" : "DMA_SDIO",
                (unsigned)sd.sdErrorCode(), (unsigned long)sd.sdErrorData());
            delay(250u * (uint32_t)a);
        }
    }
    if (!sd_ok) { halt("the SD card would not initialise; power-cycle the Teensy (unplug USB for 2 s)"); return; }
    if (!model_file_open()) { halt("qwen3b.gguf not found on the card"); return; }
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

    say("\n  opening the model: tokenizer into PSRAM, norms and biases into PSRAM, cache laid out");
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
    say("    PSRAM used %lu of %lu: tokenizer %lu, norms+biases %lu, attention cache %lu (%d positions)",
        (unsigned long)info.ps_used, (unsigned long)plat_ps_size(), (unsigned long)info.ps_tokenizer,
        (unsigned long)info.ps_small, (unsigned long)info.ps_kv, info.max_seq);
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
