/* ===========================================================================================
 *  hybrid.c -- the Luckfox and the Teensy computing at the same time instead of in turn
 *
 *  WHAT WAS WRONG WITH THE MACHINE
 *
 *  Both processors hold weights and both can multiply-accumulate, and until now exactly one of them
 *  worked at a time. The Luckfox issued a command, blocked on a read, and sat idle for seconds while a
 *  microcontroller walked its PSRAM at 3.88 MB/s -- with 648 MMAC/s of NEON and DDR2 doing nothing.
 *
 *  Nothing about that needed a faster bus to fix. The Teensy's reply arrives when it arrives; the only
 *  question is whether this end is asleep or working in the meantime. So the loop becomes:
 *
 *      issue the Teensy's block          (a dozen bytes down the UART, returns immediately)
 *      multiply-accumulate the local slice out of DDR2 with NEON
 *      collect the Teensy's partial sum
 *
 *  and the pass costs max(teensy, luckfox) instead of their sum. Only partial sums cross the wire -- a
 *  few bytes -- because each end computes on the weights it physically holds. Sending weights over a
 *  1 Mbaud UART would take eight seconds a megabyte and is the one thing this design must never do.
 *
 *  IT MEASURES BOTH WAYS. A concurrency claim with no sequential control is not a measurement, so every
 *  run does the same work twice, once overlapped and once strictly in turn, and prints both.
 *
 *  Bank settings come from /root/banks.txt, which drive.py writes after its qualification. Repeating
 *  that qualification here would be a second implementation of the one part of this system that is
 *  hardened, and two implementations of a timing rule is how this project lost an afternoon before.
 * ======================================================================================== */

#define _GNU_SOURCE
#include <arm_neon.h>
#include <asm/termbits.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define NACT     2048
#define MAXBANK  16

static int8_t  xvec[NACT];
static int64_t xsum;
static int     fd;

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

/* ---- the link -------------------------------------------------------------------------------
 *  termios2 with BOTHER, because 1 Mbaud is not one of the historical B-constants on every arch and
 *  guessing which it is here would be exactly the kind of assumption that has cost this project time.
 */
static int link_open(const char *dev)
{
    int f = open(dev, O_RDWR | O_NOCTTY);
    if (f < 0) return -1;
    struct termios2 t;
    if (ioctl(f, TCGETS2, &t) < 0) { close(f); return -1; }
    t.c_cflag &= ~CBAUD;
    t.c_cflag |= BOTHER | CS8 | CLOCAL | CREAD;
    t.c_cflag &= ~(PARENB | CSTOPB | CRTSCTS);
    t.c_iflag = 0;
    t.c_oflag = 0;
    t.c_lflag = 0;
    t.c_ispeed = 1000000;
    t.c_ospeed = 1000000;
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 5;                 /* 0.5 s granularity; the read loop owns the real deadline */
    if (ioctl(f, TCSETS2, &t) < 0) { close(f); return -1; }
    ioctl(f, TCFLSH, 2);
    return f;
}

static void link_send(const char *cmd)
{
    char buf[96];
    const int n = snprintf(buf, sizeof(buf), "%s\n", cmd);
    ssize_t off = 0;
    while (off < n) {
        const ssize_t w = write(fd, buf + off, (size_t)(n - off));
        if (w <= 0) break;
        off += w;
    }
}

/* Read lines until one begins with the letter of the command that caused it.
 *
 * A reply still in flight when the next command goes out otherwise arrives after it and is read as the
 * answer -- that produced an identity string with a "J 8" welded on and a cycle count that parsed as
 * "336293I" before it was understood. Every reply starts with its command's letter. */
static int link_recv(char want, char *out, size_t outsz, double timeout_s)
{
    static char acc[512];
    static size_t used = 0;
    const double deadline = now_s() + timeout_s;

    for (;;) {
        char *nl;
        while ((nl = (char *)memchr(acc, '\n', used)) != NULL) {
            const size_t linelen = (size_t)(nl - acc);
            char line[512];
            memcpy(line, acc, linelen);
            line[linelen] = 0;
            memmove(acc, nl + 1, used - linelen - 1);
            used -= linelen + 1;
            char *p = line;
            while (*p == ' ' || *p == '\r') p++;
            if (*p && (*p == want || *p == (want | 32) || *p == (want & ~32))) {
                snprintf(out, outsz, "%s", p);
                return 0;
            }
        }
        if (now_s() > deadline) return -1;
        if (used >= sizeof(acc) - 1) used = 0;
        const ssize_t r = read(fd, acc + used, sizeof(acc) - 1 - used);
        if (r > 0) used += (size_t)r;
    }
}

static int ask(const char *cmd, char *out, size_t outsz, double timeout_s)
{
    link_send(cmd);
    return link_recv(cmd[0], out, outsz, timeout_s);
}

/* ---- the local kernel, identical in meaning to the Teensy's ------------------------------- */
static int64_t mac1_neon(const uint8_t *w, size_t nbytes)
{
    const uint8x8_t bitsel = { 1, 2, 4, 8, 16, 32, 64, 128 };
    int32x4_t acc = vdupq_n_s32(0);
    for (size_t j = 0; j < nbytes; j += 8) {
        const int8_t *x = &xvec[(8 * j) & (NACT - 1)];
        int16x8_t part = vdupq_n_s16(0);
        for (int k = 0; k < 8; k++) {
            const uint8x8_t bcast = vdup_n_u8(w[j + k]);
            const uint8x8_t sel   = vand_u8(bcast, bitsel);
            const uint8x8_t mask  = vceq_u8(sel, bitsel);
            const int8x8_t  a     = vld1_s8(x + 8 * k);
            part = vaddw_s8(part, vand_s8(a, vreinterpret_s8_u8(mask)));
        }
        acc = vpadalq_s16(acc, part);
    }
    const int64_t set_sum = (int64_t)vgetq_lane_s32(acc, 0) + vgetq_lane_s32(acc, 1)
                          + vgetq_lane_s32(acc, 2) + vgetq_lane_s32(acc, 3);
    return 2 * set_sum - (int64_t)(nbytes / 256) * xsum;
}

/* ---- banks ---------------------------------------------------------------------------------- */
struct bank {
    int kind, y, mode, wi, ri, nb, su;
    char name[8];
};

static struct bank banks[MAXBANK];
static int nbank;

static int load_banks(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[160];
    while (nbank < MAXBANK && fgets(line, sizeof(line), f)) {
        struct bank *b = &banks[nbank];
        if (sscanf(line, "%d %d %7s %d %d %d %d %d", &b->kind, &b->y, b->name,
                   &b->mode, &b->wi, &b->ri, &b->nb, &b->su) == 8)
            nbank++;
    }
    fclose(f);
    return nbank;
}

static void bank_select(const struct bank *b)
{
    char cmd[64], rep[160];
    snprintf(cmd, sizeof(cmd), "Q %d", b->mode);        ask(cmd, rep, sizeof(rep), 5.0);
    ask("D 6", rep, sizeof(rep), 5.0);
    if (b->mode == 2) ask("J 8", rep, sizeof(rep), 5.0);
    ask("Y 0", rep, sizeof(rep), 5.0);
    snprintf(cmd, sizeof(cmd), "U %d", b->su);          ask(cmd, rep, sizeof(rep), 5.0);
    snprintf(cmd, sizeof(cmd), "B %d %d", b->kind, b->y); ask(cmd, rep, sizeof(rep), 15.0);
    snprintf(cmd, sizeof(cmd), "T %d %d %d", b->wi, b->ri, b->nb);
    ask(cmd, rep, sizeof(rep), 5.0);
}

int main(int argc, char **argv)
{
    const size_t span   = (argc > 1) ? (size_t)atoi(argv[1]) * 1024u : 1024u * 1024u;
    const int    batch  = (argc > 2) ? atoi(argv[2]) : 16;
    const size_t localm = (argc > 3) ? (size_t)atoi(argv[3]) : 8;
    const int    rounds = (argc > 4) ? atoi(argv[4]) : 3;

    for (int i = 0; i < NACT; i++) xvec[i] = (int8_t)(((i * 37) & 0x7F) - 64);
    xsum = 0;
    for (int i = 0; i < NACT; i++) xsum += xvec[i];

    if (load_banks("/root/banks.txt") <= 0) {
        printf("  no /root/banks.txt -- run drive.py first so the qualification is not repeated here\n");
        return 1;
    }

    fd = link_open("/dev/ttyS3");
    if (fd < 0) { printf("  cannot open /dev/ttyS3\n"); return 1; }

    char rep[256];
    if (ask("I", rep, sizeof(rep), 20.0) != 0) { printf("  no reply from the teensy\n"); return 1; }
    printf("  teensy: %s\n", rep);

    const size_t ln = localm * 1024u * 1024u;
    uint8_t *lw = (uint8_t *)malloc(ln);
    if (!lw) { printf("  cannot allocate %zu MB in DDR2\n", localm); return 1; }
    for (size_t i = 0; i < ln; i++) lw[i] = (uint8_t)(i * 0x9Du + 0x3Bu);

    printf("  %d teensy banks x %zu kB + %zu MB local, batch %d\n",
           nbank, span / 1024, localm, batch);
    printf("  total weights a pass: %.1f MB = %.0f million parameters\n",
           (nbank * span + ln) / 1e6, (nbank * span + ln) * 8.0 / 1e6);

    /* fill the teensy banks once */
    for (int i = 0; i < nbank; i++) {
        char cmd[64];
        bank_select(&banks[i]);
        ask("N 0", rep, sizeof(rep), 5.0);
        snprintf(cmd, sizeof(cmd), "F 0 %zu", span);
        ask(cmd, rep, sizeof(rep), 600.0);
    }

    printf("\n  round        mode   teensy s   luckfox s    wall s   MB/s   MMAC/s   tokens/s   checksum\n");
    double best_conc = 0.0, best_seq = 0.0;

    /* THE ACCUMULATOR HAS TO BE OBSERVABLE OR THE WORK IS NOT DONE.
     *
     * The first version of this loop ended with (void)total, and -O3 deleted every NEON call behind it:
     * the local side reported 0.03 s for work that cannot take less than 1.6 s, and the headline rate
     * counted bytes nobody had touched. A benchmark whose result is unused is not a benchmark. The sum
     * goes to a volatile and is printed. */
    static volatile int64_t sink;

    for (int r = 0; r < rounds; r++) {
        for (int mode = 0; mode < 2; mode++) {     /* 0 = strictly in turn, 1 = overlapped */
            int64_t total = 0;
            double t_teensy = 0.0, t_local = 0.0;
            const double t0 = now_s();

            for (int i = 0; i < nbank; i++) {
                char cmd[64];
                bank_select(&banks[i]);
                snprintf(cmd, sizeof(cmd), "M 0 %zu 1 %d", span, batch);

                if (mode == 0) {
                    const double a = now_s();
                    ask(cmd, rep, sizeof(rep), 900.0);
                    t_teensy += now_s() - a;
                    total += atoll(rep + 2);
                    const double b = now_s();
                    for (int k = 0; k < batch; k++) total += mac1_neon(lw, ln / nbank);
                    t_local += now_s() - b;
                } else {
                    /* the whole point: the command is on the wire and this core does not wait for it */
                    link_send(cmd);
                    const double b = now_s();
                    for (int k = 0; k < batch; k++) total += mac1_neon(lw, ln / nbank);
                    t_local += now_s() - b;
                    const double a = now_s();
                    if (link_recv('M', rep, sizeof(rep), 900.0) == 0) total += atoll(rep + 2);
                    t_teensy += now_s() - a;
                }
            }

            sink = total;
            const double wall  = now_s() - t0;
            const double bytes = (double)(nbank * span + ln);
            const double mbps  = bytes / wall / 1e6;
            const double mmac  = bytes * 8.0 * batch / wall / 1e6;
            const double toks  = (double)batch / wall;
            printf("  %5d  %10s   %8.2f   %9.2f  %8.2f  %5.2f  %7.1f   %8.3f  %lld\n",
                   r, mode ? "overlapped" : "in turn", t_teensy, t_local, wall, mbps, mmac,
                   toks, (long long)sink);
            if (mode) { if (mmac > best_conc) best_conc = mmac; }
            else      { if (mmac > best_seq)  best_seq  = mmac; }

        }
    }

    printf("\n  in turn     %7.1f MMAC/s\n", best_seq);
    printf("  overlapped  %7.1f MMAC/s   %.2fx\n", best_conc,
           best_seq > 0 ? best_conc / best_seq : 0.0);
    free(lw);
    close(fd);
    return 0;
}
