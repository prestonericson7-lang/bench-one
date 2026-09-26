/*
 * backend_hw.c -- the real PL through /dev/mem (SPEC sections 9, 10).
 *
 * Probe order (nothing in GP0 space is touched before step 3):
 *   1. "a bitstream is loaded" = one of
 *      a. devcfg INT_STS (0xF800700C) PCFG_DONE is set (PL configured after Linux booted: JTAG,
 *         fpga_manager), or
 *      b. the kernel command line carries fpgagpu.pl_loaded=1 (appended by the SD card's
 *         boot.scr only when U-Boot's "fpga loadb" of pl.bit succeeded) AND INT_STS shows no
 *         PL reset since Linux started (PCFG_INIT_NE, bit 0, clear).
 *      Why b: Linux's zynq-fpga driver (drivers/fpga/zynq-fpga.c, zynq_fpga_probe) writes
 *      IXR_ALL_MASK to the write-1-to-clear INT_STS at boot, so PCFG_DONE reads 0 under Linux
 *      for a bitstream U-Boot loaded -- the boot flow of this SD image. (fpga_manager's "state"
 *      is derived from the same bit and reads "unknown" then.) Any later PL clear/re-program
 *      drives INIT_B low and sets PCFG_INIT_NE, which b refuses and alive() reports as loss.
 *      If the devcfg page cannot be mapped, /sys/class/fpga_manager/fpga0/state == "operating"
 *      is used instead of a (b is not used then: no way to see a later PL reset).
 *   2. SLCR LVL_SHFTR_EN (0xF8000900) must be 0xF: with the PS-PL level shifters off every GP0
 *      access hangs the CPU. (U-Boot "fpga loadb" and Linux fpga_manager both enable them.)
 *   2b. The DDR window 0x1E000000..0x1FFFFFFF must not be Linux RAM: /proc/iomem may show no
 *      "System RAM" range overlapping it. On the 1 GB platform the window sits in the middle of
 *      RAM, so without a reserved-memory no-map node in the device tree (or when Linux refused that
 *      node, e.g. because U-Boot relocated the device tree into it) the GPU would write over pages
 *      Linux is using. No-map regions are left out of System RAM (arch/arm/kernel/setup.c
 *      request_standard_resources, for_each_mem_range skips MEMBLOCK_NOMAP).
 *   3. GP0 reads run in a forked child with a 1 s timeout, so a bitstream that answers with a bus
 *      error kills only the child and a hang is detected instead of freezing the daemon:
 *      a. the word at 0x40000000, read first as a GP0 hang check. On the repo's platform bitstream
 *         that is the platform register file pl_regs (ID 0x5A702001, hardware/pz7020-starlite/
 *         ps7-axi/pl_regs.v), which lives beside the GPU; a standalone GPU bitstream aliases the
 *         GPU's own ID there (its GP0 slave decodes only addr[11:0]). Either way the GPU is looked
 *         for at 0x43C00000 next: a bitstream without it answers there with a decode error (probe
 *         child killed) or with a different ID (pz7020_ps7_top aliases pl_regs), both refused.
 *      b. ID (0x47505531) and VERSION (major 1) at 0x43C00000.
 *   4. The 32 MB DDR window 0x1E000000 is mapped (/dev/mem opened O_SYNC -> uncached).
 * While ready, alive() re-reads INT_STS (devcfg only, no GP0 access) on every loop iteration:
 * the PL counts as lost when PCFG_DONE drops (if the probe relied on it) or PCFG_INIT_NE becomes
 * set after the probe. Limitation: these are sticky interrupt-status bits; when both were already
 * set at probe time (PL configured by JTAG after boot), a second JTAG re-program while the daemon
 * runs is not seen (stop the service first). fpga_manager re-programming clears INT_STS and is seen.
 *
 * DDR accesses use aligned 64-bit volatile loads/stores only: /dev/mem maps the no-map region
 * uncached (Strongly-ordered or Normal-NC), where unaligned accesses would fault.
 */
#include <string.h>
#include "backend.h"
#include "gpu_proto.h"

#define SLCR_BASE          0xF8000000u
#define SLCR_LVL_SHFTR_EN  0x900u
#define GP0_ALIAS          0x40000000u
#define PLATFORM_REGS_ID   0x5A702001u      /* platform pl_regs at 0x40000000 (pl_regs.py ID_EXPECTED) */
#define FPGA_MGR_STATE     "/sys/class/fpga_manager/fpga0/state"
#define IOMEM_PATH         "/proc/iomem"
#define BOOT_MARKER        "fpgagpu.pl_loaded=1"
#define DEVCFG_INIT_NE     (1u << 0)        /* INT_STS PCFG_INIT_NE: PL INIT_B went low (PL reset) */
#define PAGE               4096u

typedef struct {
    backend             base;
    const char         *fake;           /* fake /dev/mem file (tests) or NULL */
    const char         *cmdline;        /* /proc/cmdline (tests: a file) */
    const char         *iomem;          /* /proc/iomem (tests: a file; NULL = check skipped) */
    int                 boot_marker;    /* -1 = not read yet, 0/1 */
    int                 via_pcfg;       /* the last successful probe saw PCFG_DONE */
    uint32_t            sts0;           /* INT_STS at the last successful probe */
    int                 fd;
    volatile uint32_t  *devcfg;
    int                 devcfg_failed;
    volatile uint32_t  *slcr;
    int                 slcr_failed;
    volatile uint32_t  *alias;
    uint32_t            alias_seen;     /* last value read at 0x40000000 (logged when it changes) */
    int                 alias_logged;
    volatile uint32_t  *regs;
    volatile uint8_t   *ddr;
    int                 hung;           /* a probe read hung: never probe again */
    int                 hung_pid;
    uint64_t            sysfs_next;
    int                 sysfs_ok;
} hw;

static uint64_t phys_to_off(const hw *h, uint32_t phys)
{
    if (!h->fake)
        return phys;
    switch (phys) {
    case ZYNQ_DEVCFG_BASE: return 0x0000;
    case SLCR_BASE:        return 0x1000;
    case GP0_ALIAS:        return 0x2000;
    case GPU_REGS_PHYS:    return 0x3000;
    case GPU_DDR_BASE:     return 0x4000;
    default:               return 0xFFFFF000u;   /* not reached */
    }
}

static void *map_phys(hw *h, uint32_t phys, size_t len, int writable, char *why, size_t whyn)
{
    int err = 0;
    void *p = os_mmap(h->fd, phys_to_off(h, phys), len, writable, &err);
    if (!p && why)
        os_snprintf(why, whyn, "mmap of physical 0x%08x (+0x%zx) failed: %s", phys, len,
                    os_strerror(err));
    return p;
}

/* 1 = the kernel command line has BOOT_MARKER as a whole word (read once: it cannot change) */
static int boot_marker(hw *h)
{
    char buf[4096];
    const char *p;
    size_t ml = sizeof BOOT_MARKER - 1;
    if (h->boot_marker >= 0)
        return h->boot_marker;
    h->boot_marker = 0;
    if (!h->cmdline)
        return 0;
    if (os_read_text(h->cmdline, buf, sizeof buf) <= 0) {
        os_log("cannot read %s; assuming U-Boot did not load pl.bit", h->cmdline);
        return 0;
    }
    for (p = buf; *p; p++) {
        if ((p == buf || p[-1] == ' ') && strncmp(p, BOOT_MARKER, ml) == 0 &&
            (p[ml] == 0 || p[ml] == ' ' || p[ml] == '\n')) {
            h->boot_marker = 1;
            break;
        }
    }
    os_log("kernel command line (%s) %s %s: U-Boot %s pl.bit", h->cmdline,
           h->boot_marker ? "has" : "does not have", BOOT_MARKER,
           h->boot_marker ? "loaded" : "did not report loading");
    return h->boot_marker;
}

/* Whole file into buf, NUL-terminated (a /proc file can need several reads). Bytes or -errno. */
static int read_all(const char *path, char *buf, size_t n)
{
    size_t got = 0;
    int fd = os_open(path, 0, 0);
    if (fd < 0)
        return fd;
    while (got + 1 < n) {
        long r = os_read(fd, buf + got, n - 1 - got);
        if (r == -OS_EINTR)
            continue;
        if (r < 0) {
            os_close(fd);
            return (int)r;
        }
        if (r == 0)
            break;
        got += (size_t)r;
    }
    os_close(fd);
    buf[got] = 0;
    return (int)got;
}

static int parse_hex(const char **pp, uint64_t *out)
{
    const char *p = *pp;
    uint64_t v = 0;
    int n = 0, d;
    for (;; p++, n++) {
        if (*p >= '0' && *p <= '9')
            d = *p - '0';
        else if (*p >= 'a' && *p <= 'f')
            d = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F')
            d = *p - 'A' + 10;
        else
            break;
        if (n >= 16)
            return -1;
        v = v << 4 | (uint64_t)d;
    }
    if (n == 0)
        return -1;
    *pp = p;
    *out = v;
    return 0;
}

/* 1 = no "System RAM" range in /proc/iomem overlaps the GPU's DDR window (step 2b), 0 = it does or
 * that cannot be verified (reason in why). Skipped (1) when h->iomem is NULL (fake devmem tests). */
static int window_reserved(hw *h, char *why, size_t whyn)
{
    static char buf[32768];
    const uint64_t lo = GPU_DDR_BASE, hi = (uint64_t)GPU_DDR_BASE + GPU_DDR_SIZE - 1u;
    const char *p;
    int n, seen = 0, nonzero = 0;
    if (!h->iomem)
        return 1;
    n = read_all(h->iomem, buf, sizeof buf);
    if (n < 0 || (size_t)n >= sizeof buf - 1u) {
        os_snprintf(why, whyn, "cannot read %s (%s): cannot verify that the GPU's DDR window 0x%08x..0x%08x "
                    "is reserved -- not touching the PL", h->iomem, n < 0 ? os_strerror(n) : "too large",
                    GPU_DDR_BASE, (uint32_t)hi);
        return 0;
    }
    for (p = buf; *p;) {
        const char *q = p;
        uint64_t s, e;
        while (*p && *p != '\n')
            p++;
        if (*p)
            p++;
        if (*q == ' ')
            continue;                               /* nested resource (Kernel code, ...) */
        if (parse_hex(&q, &s) < 0 || *q != '-')
            continue;
        q++;
        if (parse_hex(&q, &e) < 0 || strncmp(q, " : System RAM", 13) != 0 || (q[13] != '\n' && q[13] != 0))
            continue;
        seen++;
        if (s || e)
            nonzero = 1;
        if (s <= hi && e >= lo) {
            os_snprintf(why, whyn, "the GPU's DDR window 0x%08x..0x%08x is Linux RAM (%s: %08llx-%08llx : "
                        "System RAM): the device tree must reserve it (reserved-memory gpu@1e000000, "
                        "no-map) -- not touching the PL", GPU_DDR_BASE, (uint32_t)hi, h->iomem,
                        (unsigned long long)s, (unsigned long long)e);
            return 0;
        }
    }
    if (!seen || !nonzero) {
        os_snprintf(why, whyn, "%s lists no System RAM addresses (not running as root?): cannot verify that "
                    "the GPU's DDR window is reserved -- not touching the PL", h->iomem);
        return 0;
    }
    return 1;
}

/* 1 = a bitstream is loaded (see the header comment, step 1). *via_pcfg = PCFG_DONE was set. */
static int pl_configured(hw *h, uint32_t *sts, int *via_pcfg, char *why, size_t whyn)
{
    char buf[64];
    *sts = 0;
    *via_pcfg = 0;
    if (h->devcfg) {
        *sts = h->devcfg[ZYNQ_DEVCFG_INT_STS / 4];
        if (*sts & ZYNQ_DEVCFG_PCFG_DONE) {
            *via_pcfg = 1;
            return 1;
        }
        if (boot_marker(h)) {
            if (!(*sts & DEVCFG_INIT_NE))
                return 1;
            os_snprintf(why, whyn, "PL was reset after U-Boot loaded pl.bit and is not configured "
                        "(devcfg INT_STS=0x%08x: PCFG_INIT_NE set, PCFG_DONE=0)", *sts);
            return 0;
        }
        os_snprintf(why, whyn, "PL not configured (devcfg INT_STS=0x%08x, PCFG_DONE=0, and the kernel "
                    "command line has no %s from boot.scr)", *sts, BOOT_MARKER);
        return 0;
    }
    if (!h->fake && os_read_text(FPGA_MGR_STATE, buf, sizeof buf) > 0 && strncmp(buf, "operating", 9) == 0) {
        *via_pcfg = 1;
        return 1;
    }
    os_snprintf(why, whyn, "PL not configured (%s is not \"operating\")", FPGA_MGR_STATE);
    return 0;
}

/* Read n (<= 4) words at p in a child process. 0 = ok, -1 = the read faulted, -2 = it hung. */
static int guarded_read(hw *h, volatile uint32_t *p, int n, uint32_t phys, uint32_t *out,
                        char *why, size_t whyn)
{
    uint32_t v[4] = { 0, 0, 0, 0 };
    size_t want = (size_t)n * 4u, got = 0;
    int fds[2], pid, st = 0, i, eof = 0;
    uint64_t deadline;

    if (os_pipe(fds) < 0 || (pid = os_fork()) < 0) {
        /* cannot isolate (should not happen): read directly */
        os_log("warning: cannot fork for an isolated probe read; reading 0x%08x directly", phys);
        for (i = 0; i < n; i++)
            out[i] = p[i];
        return 0;
    }
    if (pid == 0) {
        /* the child keeps no copy of the listening sockets etc.: if the read hangs the core, a
         * restarted daemon can still bind its ports (mappings stay valid without their fd) */
        for (i = 0; i < 1024; i++)
            if (i != fds[1] && i != 2)
                os_close(i);
        os_no_coredump();
        for (i = 0; i < n; i++)
            v[i] = p[i];
        os_write(fds[1], v, want);
        os_exit(0);
    }
    os_close(fds[1]);
    deadline = os_now_ns() + 1000000000ull;
    while (got < want) {
        struct os_pollfd pf;
        uint64_t now = os_now_ns();
        long k;
        int r;
        if (now >= deadline)
            break;
        pf.fd = fds[0];
        pf.events = OS_POLLIN;
        pf.revents = 0;
        r = os_poll(&pf, 1, (int64_t)(deadline - now));
        if (r < 0 && r != -OS_EINTR)
            break;
        if (r <= 0)
            continue;
        k = os_read(fds[0], (char *)v + got, want - got);
        if (k == 0) {
            eof = 1;
            break;
        }
        if (k < 0) {
            if (k == -OS_EINTR || k == -OS_EAGAIN)
                continue;
            eof = 1;
            break;
        }
        got += (size_t)k;
    }
    os_close(fds[0]);
    if (got == want) {
        os_wait(pid, &st, 0);
        memcpy(out, v, want);
        return 0;
    }
    if (eof) {
        /* the child closed the pipe without data: it died (bus error / external abort) */
        os_wait(pid, &st, 0);
        os_snprintf(why, whyn, "reading physical 0x%08x faulted (probe child killed by signal %d); "
                    "the loaded bitstream does not answer there", phys, os_status_signal(st));
        return -1;
    }
    os_kill(pid, OS_SIGKILL);
    if (os_wait(pid, &st, 1) != pid)
        h->hung_pid = pid;
    h->hung = 1;
    os_snprintf(why, whyn, "reading physical 0x%08x did not complete within 1 s (AXI GP0 hang: the "
                "loaded bitstream does not answer there); not probing again -- load the GPU "
                "bitstream and restart fpgagpud", phys);
    return -2;
}

static int hw_probe(backend *b, char *why, size_t whyn)
{
    hw *h = (hw *)b;
    uint32_t sts = 0, v[2];
    int r, via_pcfg = 0;

    if (h->hung_pid > 0 && os_wait(h->hung_pid, &r, 1) == h->hung_pid)
        h->hung_pid = 0;
    if (h->hung) {
        os_snprintf(why, whyn, "an earlier probe read of GP0 hung; restart fpgagpud after loading "
                    "the GPU bitstream");
        return 0;
    }
    if (h->fd < 0) {
        const char *path = h->fake ? h->fake : "/dev/mem";
        int fd = os_open(path, 1, h->fake ? 0 : 1);
        if (fd < 0) {
            os_snprintf(why, whyn, "cannot open %s: %s", path, os_strerror(fd));
            return 0;
        }
        h->fd = fd;
    }

    /* 1. PCFG_DONE */
    if (!h->devcfg && !h->devcfg_failed) {
        char w2[160];
        h->devcfg = map_phys(h, ZYNQ_DEVCFG_BASE, PAGE, 0, w2, sizeof w2);
        if (!h->devcfg) {
            h->devcfg_failed = 1;
            os_log("devcfg registers not mappable (%s); using %s instead", w2, FPGA_MGR_STATE);
        }
    }
    if (!pl_configured(h, &sts, &via_pcfg, why, whyn))
        return 0;

    /* 2. PS-PL level shifters */
    if (!h->slcr && !h->slcr_failed) {
        char w2[160];
        h->slcr = map_phys(h, SLCR_BASE, PAGE, 0, w2, sizeof w2);
        if (!h->slcr) {
            h->slcr_failed = 1;
            os_log("warning: SLCR not mappable (%s); level-shifter check skipped", w2);
        }
    }
    if (h->slcr) {
        uint32_t lvl = h->slcr[SLCR_LVL_SHFTR_EN / 4];
        if ((lvl & 0xFu) != 0xFu) {
            os_snprintf(why, whyn, "PS-PL level shifters are off (SLCR LVL_SHFTR_EN=0x%x, need 0xF); "
                        "GP0 accesses would hang", lvl);
            return 0;
        }
    }

    /* 2b. the DDR window must be reserved (not Linux RAM) before anything can make the PL write it */
    if (!window_reserved(h, why, whyn))
        return 0;

    /* 3a. what answers at the bottom of GP0 space (hang check; the platform's pl_regs lives there) */
    if (!h->alias)
        h->alias = map_phys(h, GP0_ALIAS, PAGE, 0, NULL, 0);
    if (h->alias) {
        r = guarded_read(h, h->alias, 1, GP0_ALIAS, v, why, whyn);
        if (r == -2)
            return 0;
        if (r < 0)
            os_log("note: %s -- trying 0x%08x anyway", why, GPU_REGS_PHYS);
        else if (!h->alias_logged || v[0] != h->alias_seen) {
            h->alias_logged = 1;
            h->alias_seen = v[0];
            os_log("0x%08x reads 0x%08x%s; looking for the FPGA-GPU at 0x%08x", GP0_ALIAS, v[0],
                   v[0] == PLATFORM_REGS_ID ? " (the platform register file pl_regs)" :
                   v[0] == GPU_ID_VALUE ? " (FPGA-GPU ID alias)" : "", GPU_REGS_PHYS);
        }
    }

    /* 3b. ID / VERSION */
    if (!h->regs) {
        h->regs = map_phys(h, GPU_REGS_PHYS, GPU_REGS_SIZE, 1, why, whyn);
        if (!h->regs)
            return 0;
    }
    r = guarded_read(h, h->regs, 2, GPU_REGS_PHYS, v, why, whyn);
    if (r < 0)
        return 0;
    if (v[0] != GPU_ID_VALUE) {
        os_snprintf(why, whyn, "ID register reads 0x%08x, expected 0x%08x ('GPU1'): the loaded "
                    "bitstream is not the FPGA-GPU", v[0], GPU_ID_VALUE);
        return 0;
    }
    if ((v[1] >> 16) != (GPU_VERSION_VALUE >> 16)) {
        os_snprintf(why, whyn, "VERSION 0x%08x: major %u not supported (this daemon speaks %u)",
                    v[1], v[1] >> 16, GPU_VERSION_VALUE >> 16);
        return 0;
    }

    /* 4. DDR window */
    if (!h->ddr) {
        h->ddr = map_phys(h, GPU_DDR_BASE, GPU_DDR_SIZE, 1, why, whyn);
        if (!h->ddr)
            return 0;
    }
    h->via_pcfg = via_pcfg;
    h->sts0 = sts;
    os_snprintf(why, whyn, "FPGA-GPU ID 0x%08x VERSION 0x%08x at 0x%08x, DDR window 0x%08x+0x%x%s "
                "(PL configured: %s, INT_STS=0x%08x)%s", v[0], v[1], GPU_REGS_PHYS, GPU_DDR_BASE,
                GPU_DDR_SIZE, h->iomem ? " reserved" : "", via_pcfg ? "PCFG_DONE" : "U-Boot " BOOT_MARKER,
                sts, h->fake ? " (fake devmem)" : "");
    return 1;
}

static int hw_alive(backend *b)
{
    hw *h = (hw *)b;
    uint32_t sts;
    int via;
    char why[8];
    if (h->devcfg) {
        sts = h->devcfg[ZYNQ_DEVCFG_INT_STS / 4];
        if (h->via_pcfg && !(sts & ZYNQ_DEVCFG_PCFG_DONE))
            return 0;                               /* INT_STS cleared: fpga_manager re-programs */
        if (sts & ~h->sts0 & DEVCFG_INIT_NE)
            return 0;                               /* the PL was reset since the probe */
        return 1;
    }
    if (h->fake)
        return 1;
    {
        uint64_t now = os_now_ns();
        if (now >= h->sysfs_next) {
            h->sysfs_ok = pl_configured(h, &sts, &via, why, sizeof why);
            h->sysfs_next = now + 250000000ull;
        }
    }
    return h->sysfs_ok;
}

static uint32_t hw_rd(backend *b, uint32_t off)
{
    hw *h = (hw *)b;
    return h->regs[(off & (GPU_REGS_SIZE - 1u)) >> 2];
}

static void hw_wr(backend *b, uint32_t off, uint32_t v)
{
    hw *h = (hw *)b;
    h->regs[(off & (GPU_REGS_SIZE - 1u)) >> 2] = v;
}

static void hw_push(backend *b, const uint32_t *recs, int n)
{
    hw *h = (hw *)b;
    volatile uint32_t *sor = &h->regs[GPU_R_PS_FIFO_SOR >> 2];
    volatile uint32_t *dat = &h->regs[GPU_R_PS_FIFO_DATA >> 2];
    int i, k;
    for (i = 0; i < n; i++) {
        const uint32_t *r = recs + (size_t)i * GPU_REC_WORDS;
        *sor = r[0];
        for (k = 1; k < GPU_REC_WORDS; k++)
            *dat = r[k];
    }
    os_io_barrier();
}

static int ddr_ok(uint32_t phys, size_t len)
{
    return phys >= GPU_DDR_BASE && len <= GPU_DDR_SIZE && phys - GPU_DDR_BASE <= GPU_DDR_SIZE - len
        && (phys & 7u) == 0 && (len & 7u) == 0;
}

static int hw_ddr_write(backend *b, uint32_t phys, const void *src, size_t len)
{
    hw *h = (hw *)b;
    volatile uint64_t *d;
    const uint8_t *s = src;
    size_t i;
    if (!h->ddr || !ddr_ok(phys, len))
        return -OS_EINVAL;
    d = (volatile uint64_t *)(void *)(h->ddr + (phys - GPU_DDR_BASE));
    for (i = 0; i < len / 8; i++) {
        uint64_t v;
        memcpy(&v, s + i * 8, 8);
        d[i] = v;
    }
    os_io_barrier();
    return 0;
}

static int hw_ddr_read(backend *b, uint32_t phys, void *dst, size_t len)
{
    hw *h = (hw *)b;
    const volatile uint64_t *s;
    uint8_t *d = dst;
    size_t i;
    if (!h->ddr || !ddr_ok(phys, len))
        return -OS_EINVAL;
    s = (const volatile uint64_t *)(const void *)(h->ddr + (phys - GPU_DDR_BASE));
    for (i = 0; i < len / 8; i++) {
        uint64_t v = s[i];
        memcpy(d + i * 8, &v, 8);
    }
    return 0;
}

static void hw_destroy(backend *b)
{
    hw *h = (hw *)b;
    os_munmap((void *)(uintptr_t)h->devcfg, PAGE);
    os_munmap((void *)(uintptr_t)h->slcr, PAGE);
    os_munmap((void *)(uintptr_t)h->alias, PAGE);
    os_munmap((void *)(uintptr_t)h->regs, GPU_REGS_SIZE);
    os_munmap((void *)(uintptr_t)h->ddr, GPU_DDR_SIZE);
    if (h->fd >= 0)
        os_close(h->fd);
    os_free(h);
}

static const backend_ops hw_ops = {
    hw_probe, hw_alive, hw_rd, hw_wr, hw_push, hw_ddr_write, hw_ddr_read, NULL, NULL, hw_destroy
};

backend *backend_hw_create(const char *fake_devmem, const char *cmdline, const char *iomem)
{
    hw *h = os_calloc(sizeof *h);
    if (!h)
        return NULL;
    h->base.ops = &hw_ops;
    h->base.name = fake_devmem ? "hardware (fake devmem file)" : "hardware (/dev/mem)";
    h->fake = fake_devmem;
    h->cmdline = cmdline ? cmdline : fake_devmem ? NULL : "/proc/cmdline";
    h->iomem = iomem ? iomem : fake_devmem ? NULL : IOMEM_PATH;
    h->boot_marker = -1;
    h->fd = -1;
    return &h->base;
}
