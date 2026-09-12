/* ===========================================================================================
 *  leafd.c -- a Luckfox as a worker node on the chain
 *
 *  WHAT WAS MISSING
 *
 *  The head Luckfox orchestrates. The other four were drawn on the wiring diagram, given addresses by
 *  the enumeration and then had nothing to run: no program on them listened to UART3, so they would
 *  have answered nothing and the fleet would have counted four dead nodes. This is that program.
 *
 *  It speaks the same command set as every other node, so the host cannot tell a Luckfox leaf from a
 *  Teensy except by what it says when asked. That is deliberate: the host addresses nodes, not board
 *  types.
 *
 *      A <n>       take n if unnumbered and answer. A leaf has nothing behind it, so it passes nothing on.
 *      @<n> <cmd>  run cmd if n is mine; otherwise ignore it completely.
 *      I           identity, including how many bytes of DDR2 this node holds
 *      F <a> <n>   materialise weights from the rule -- they never cross the wire
 *      V <a> <n>   read them back and count what came back wrong
 *      M <a> <n> 1 read and multiply-accumulate at one bit a weight
 *
 *  A LEAF IGNORES WHAT IS NOT ITS OWN, RATHER THAN RELAYING IT
 *
 *  It hangs off a Teensy's Serial3 and has nothing downstream. The Teensy only ever sends it lines
 *  addressed to it, but a stray broadcast must not produce a reply, because two nodes answering one
 *  question is how a chain gets out of step.
 *
 *  THE ARITHMETIC is the NEON kernel measured at 648 MMAC/s on this hardware, unchanged: broadcast the
 *  weight byte over eight lanes, AND with the bit selectors, compare-equal for a mask, AND that into the
 *  activations, widen-accumulate. Six operations for eight weights, and the same integer a Teensy would
 *  have produced, so partial sums from the two add.
 *
 *  Build with the cross compiler in tools/ and run it from the Luckfox rc.local so a leaf comes up
 *  ready without anyone logging in.
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
#define LINKDEV  "/dev/ttyS3"

static int8_t   xvec[NACT];
static int64_t  xsum;
static int      fd;
static uint8_t *weights;
static size_t   wbytes;
static int      g_addr;

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

/* termios2 with BOTHER: 1 Mbaud is not one of the historical B-constants on every architecture, and
 * guessing which it is here is exactly the sort of assumption that has cost this project time. */
static int link_open(void)
{
    int f = open(LINKDEV, O_RDWR | O_NOCTTY);
    if (f < 0) return -1;
    struct termios2 t;
    if (ioctl(f, TCGETS2, &t) < 0) { close(f); return -1; }
    t.c_cflag &= ~CBAUD;
    t.c_cflag |= BOTHER | CS8 | CLOCAL | CREAD;
    t.c_cflag &= ~(PARENB | CSTOPB | CRTSCTS);
    t.c_iflag = 0; t.c_oflag = 0; t.c_lflag = 0;
    t.c_ispeed = 1000000; t.c_ospeed = 1000000;
    t.c_cc[VMIN] = 0; t.c_cc[VTIME] = 1;
    if (ioctl(f, TCSETS2, &t) < 0) { close(f); return -1; }
    ioctl(f, TCFLSH, 2);
    return f;
}

static void say(const char *s)
{
    char buf[256];
    const int n = snprintf(buf, sizeof(buf), "%s\n", s);
    ssize_t off = 0;
    while (off < n) {
        const ssize_t w = write(fd, buf + off, (size_t)(n - off));
        if (w <= 0) break;
        off += w;
    }
}

/* the same rule every node fills its memory with, so the host can predict any node's answer */
static inline uint8_t pat(uint32_t i) { return (uint8_t)(i * 0x9Du + 0x3Bu); }

static int64_t mac1_neon(const uint8_t *w, size_t nbytes)
{
    const uint8x8_t bitsel = { 1, 2, 4, 8, 16, 32, 64, 128 };
    int32x4_t acc = vdupq_n_s32(0);
    for (size_t j = 0; j + 8 <= nbytes; j += 8) {
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

static long arg(const char *s, int which)
{
    int seen = 0;
    for (const char *p = s; *p; p++)
        if (*p == ' ') { seen++; if (seen == which) return atol(p + 1); }
    return 0;
}

static void handle(const char *c)
{
    char out[192];

    /* Addressed traffic. A leaf has nothing behind it, so anything not for this node is dropped in
     * silence -- two nodes answering one question is how a chain gets out of step. */
    if (c[0] == '@') {
        const char *sp = c;
        while (*sp && *sp != ' ') sp++;
        if (!g_addr || atol(c + 1) != g_addr || !*sp) return;
        c = sp + 1;
    }

    if ((c[0] == 'A' || c[0] == 'a') && (c[1] == ' ' || c[1] == 0)) {
        const long want = atol(c + 1);
        if (!g_addr && want > 0) {
            g_addr = (int)want;
            snprintf(out, sizeof(out), "A %d here leaf 0", g_addr);
            say(out);
        }
        return;                       /* nothing downstream to offer the next number to */
    }

    switch (c[0]) {
    case 'I': case 'i':
        snprintf(out, sizeof(out),
                 "I bench-one luckfox 1 addr %d psram %lu bits 1 fcpu 1000000",
                 g_addr, (unsigned long)wbytes);
        say(out);
        break;

    case 'F': case 'f': {
        uint32_t a = (uint32_t)arg(c, 1), len = (uint32_t)arg(c, 2);
        if (a > wbytes) a = 0;
        if (a + len > wbytes) len = (uint32_t)(wbytes - a);
        const double t0 = now_s();
        for (uint32_t i = 0; i < len; i++) weights[a + i] = pat(a + i);
        snprintf(out, sizeof(out), "F %lu", (unsigned long)((now_s() - t0) * 1e6));
        say(out);
        break;
    }

    case 'V': case 'v': {
        uint32_t a = (uint32_t)arg(c, 1), len = (uint32_t)arg(c, 2);
        if (a > wbytes) a = 0;
        if (a + len > wbytes) len = (uint32_t)(wbytes - a);
        uint32_t bad = 0;
        const double t0 = now_s();
        for (uint32_t i = 0; i < len; i++) if (weights[a + i] != pat(a + i)) bad++;
        snprintf(out, sizeof(out), "V %lu %lu",
                 (unsigned long)bad, (unsigned long)((now_s() - t0) * 1e6));
        say(out);
        break;
    }

    case 'M': case 'm': {
        uint32_t a = (uint32_t)arg(c, 1), len = (uint32_t)arg(c, 2);
        const long bits = arg(c, 3);
        if (a > wbytes) a = 0;
        if (a + len > wbytes) len = (uint32_t)(wbytes - a);
        if (bits != 1) { say("E bits"); break; }
        const double t0 = now_s();
        const int64_t sum = mac1_neon(weights + a, len);
        snprintf(out, sizeof(out), "M %lld %lu",
                 (long long)sum, (unsigned long)((now_s() - t0) * 1e6));
        say(out);
        break;
    }

    /* the knobs a Teensy needs and a Luckfox does not. Answered so the host's sequence does not stall
     * waiting for a node that simply has no bus to configure. */
    case 'Q': case 'q': case 'D': case 'd': case 'J': case 'j':
    case 'K': case 'k': case 'U': case 'u': case 'Y': case 'y':
    case 'T': case 't': case 'B': case 'b': case 'N': case 'n':
    case 'E': case 'e':
        snprintf(out, sizeof(out), "%c ok", c[0]);
        say(out);
        break;

    default:
        say("E unknown");
        break;
    }
}

int main(int argc, char **argv)
{
    size_t want_mb = (argc > 1) ? (size_t)atoi(argv[1]) : 12;

    for (int i = 0; i < NACT; i++) xvec[i] = (int8_t)(((i * 37) & 0x7F) - 64);
    xsum = 0;
    for (int i = 0; i < NACT; i++) xsum += xvec[i];

    /* Take what will actually allocate. Asking for more than the board has and dying is worse than
     * being a smaller node: the fleet allocates by what each node reports it holds. */
    while (want_mb > 0) {
        wbytes = want_mb * 1024u * 1024u;
        weights = (uint8_t *)malloc(wbytes);
        if (weights) break;
        want_mb -= 1;
    }
    if (!weights) { fprintf(stderr, "leafd: no memory for weights\n"); return 1; }
    memset(weights, 0, wbytes);

    fd = link_open();
    if (fd < 0) { fprintf(stderr, "leafd: cannot open %s\n", LINKDEV); return 1; }

    fprintf(stderr, "leafd: %zu MB of weights = %zu million parameters at one bit, waiting for A\n",
            wbytes / (1024 * 1024), wbytes * 8 / 1000000);

    char line[192];
    size_t ln = 0;
    for (;;) {
        char ch;
        const ssize_t r = read(fd, &ch, 1);
        if (r <= 0) continue;
        if (ch == 0x0A || ch == 0x0D) {
            if (ln) { line[ln] = 0; handle(line); ln = 0; }
        } else if (ln < sizeof(line) - 1) {
            line[ln++] = ch;
        }
    }
}
