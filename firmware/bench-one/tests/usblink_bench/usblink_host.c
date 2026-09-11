/* ===========================================================================================
 *  usblink_host.c -- the Luckfox side of the Teensy link benchmark
 * ===========================================================================================
 *
 *  Runs on the Luckfox (or any Linux host) against a Teensy running usblink_teensy.ino, and answers
 *  the one question that decides whether a Luckfox can usefully feed a Teensy: how many megabytes per
 *  second crosses the link.
 *
 *  WHAT IT HAS TO BEAT
 *      Teensy reading its own PSRAM, measured        18.2 MB/s effective
 *      Teensy reading DDR3 via the FPGA, cfgB        15.2 MB/s effective
 *  Below roughly 20 MB/s, forwarding a weight over this link is slower than the Teensy fetching that
 *  weight itself, and the Luckfox is a bottleneck rather than a helper.
 *
 *  THE TRAP THAT RUINS THIS MEASUREMENT
 *  -----------------------------------
 *  A Linux CDC-ACM port comes up as a terminal, not a pipe. In its default line discipline it
 *  translates newlines, expands tabs, strips the eighth bit on some settings, and treats 0x11 and 0x13
 *  as flow control -- so binary data arrives altered and the throughput figure is measured over a
 *  corrupted channel. cfmakeraw() plus VMIN/VTIME is not tuning, it is the difference between a
 *  measurement and a fiction. The Teensy verifies every byte it receives for exactly this reason; if
 *  it reports mismatches, this is why.
 *
 *  Baud rate is meaningless on USB CDC. The setting below exists because termios requires one.
 *
 *  BUILD
 *      cc -O2 -o usblink_host usblink_host.c
 *  RUN
 *      ./usblink_host /dev/ttyACM0 [megabytes]
 * ========================================================================================= */

#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>
#include <time.h>

static uint8_t pat(uint32_t i) { return (uint8_t)(i * 0x9Du + 0x3Bu); }

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int open_raw(const char *dev)
{
    int fd = open(dev, O_RDWR | O_NOCTTY);
    if (fd < 0) { perror(dev); return -1; }

    struct termios t;
    if (tcgetattr(fd, &t) != 0) { perror("tcgetattr"); close(fd); return -1; }
    cfmakeraw(&t);                 /* the whole point: no line discipline, no translation */
    t.c_cflag |= CLOCAL | CREAD;
    t.c_cflag &= ~CRTSCTS;
    t.c_cc[VMIN]  = 0;
    t.c_cc[VTIME] = 10;            /* 1 s read timeout, so a wedged link reports instead of hanging */
    cfsetispeed(&t, B115200);
    cfsetospeed(&t, B115200);
    if (tcsetattr(fd, TCSANOW, &t) != 0) { perror("tcsetattr"); close(fd); return -1; }
    tcflush(fd, TCIOFLUSH);
    return fd;
}

static int write_all(int fd, const void *p, size_t n)
{
    const uint8_t *b = p;
    size_t done = 0;
    while (done < n) {
        ssize_t w = write(fd, b + done, n - done);
        if (w < 0) { if (errno == EINTR) continue; perror("write"); return -1; }
        if (w == 0) return -1;
        done += (size_t)w;
    }
    return 0;
}

/* Read until a newline or the buffer fills. Used only for the Teensy's short text replies. */
static int read_line(int fd, char *out, size_t cap)
{
    size_t i = 0;
    while (i + 1 < cap) {
        uint8_t c;
        ssize_t r = read(fd, &c, 1);
        if (r <= 0) break;
        if (c == '\n') break;
        if (c != '\r') out[i++] = (char)c;
    }
    out[i] = 0;
    return (int)i;
}

static int cmd_count(int fd, char c, uint32_t n)
{
    uint8_t hdr[5];
    hdr[0] = (uint8_t)c;
    hdr[1] = (uint8_t)(n      ); hdr[2] = (uint8_t)(n >>  8);
    hdr[3] = (uint8_t)(n >> 16); hdr[4] = (uint8_t)(n >> 24);
    return write_all(fd, hdr, 5);
}

#define BLK 4096

int main(int argc, char **argv)
{
    const char *dev = (argc > 1) ? argv[1] : "/dev/ttyACM0";
    uint32_t mb = (argc > 2) ? (uint32_t)strtoul(argv[2], NULL, 10) : 8u;
    uint32_t total = mb << 20;

    int fd = open_raw(dev);
    if (fd < 0) return 1;

    static uint8_t buf[BLK];
    char line[256];

    /* ---- handshake, which also proves the port is raw enough to carry a reply ---- */
    if (write_all(fd, "V", 1) < 0) return 1;
    if (read_line(fd, line, sizeof(line)) <= 0) {
        fprintf(stderr, "no reply from %s.\n", dev);
        fprintf(stderr, "  - is the Teensy running usblink_teensy.ino?\n");
        fprintf(stderr, "  - is something else holding the port (ModemManager, a serial monitor)?\n");
        return 1;
    }
    printf("link to %s: %s\n", dev, line);
    printf("streaming %u MB each way\n\n", mb);

    /* =====================================================================================
     *  IN: host -> Teensy. The direction that matters, because this is weights arriving.
     * ================================================================================== */
    if (cmd_count(fd, 'I', total) < 0) return 1;
    double t0 = now_s();
    for (uint32_t off = 0; off < total; off += BLK) {
        uint32_t n = total - off; if (n > BLK) n = BLK;
        for (uint32_t i = 0; i < n; i++) buf[i] = pat(off + i);
        if (write_all(fd, buf, n) < 0) return 1;
    }
    double t_in = now_s() - t0;

    /* The Teensy's own timing is reported too. Two clocks measuring one transfer disagree by the
     * command latency, and seeing both makes that visible instead of arguable. */
    unsigned long got = 0, bad = 0, us = 0;
    if (read_line(fd, line, sizeof(line)) > 0)
        sscanf(line, "ok %lu %lu %lu", &got, &bad, &us);

    printf("host -> Teensy   %6.2f MB/s   (host clock)\n", (double)total / t_in / 1048576.0);
    if (us)
        printf("                 %6.2f MB/s   (Teensy clock)\n",
               (double)got / (double)us * 1e6 / 1048576.0);
    printf("                 %lu of %u bytes arrived, %lu wrong\n", got, total, bad);
    if (bad)
        printf("                 *** MISMATCHES. The port is not raw, or bytes were dropped.\n"
               "                     A throughput number over a corrupting link means nothing.\n");
    if (got < total)
        printf("                 *** SHORT. The Teensy stopped receiving before the end.\n");
    printf("\n");

    /* =====================================================================================
     *  OUT: Teensy -> host. Results leaving. Small in the real workload.
     * ================================================================================== */
    if (cmd_count(fd, 'O', total) < 0) return 1;
    uint32_t rx = 0, rbad = 0;
    t0 = now_s();
    while (rx < total) {
        ssize_t r = read(fd, buf, sizeof(buf));
        if (r <= 0) break;
        for (ssize_t i = 0; i < r; i++) if (buf[i] != pat(rx + (uint32_t)i)) rbad++;
        rx += (uint32_t)r;
    }
    double t_out = now_s() - t0;
    printf("Teensy -> host   %6.2f MB/s\n", (double)rx / t_out / 1048576.0);
    printf("                 %u of %u bytes arrived, %u wrong\n", rx, total, rbad);
    printf("\n");

    /* =====================================================================================
     *  RTT: the per-message cost, which is a different quantity from the per-byte cost.
     *
     *  This is what decides whether the Luckfox can serve a Teensy on demand or has to push in bulk.
     *  A 150 us round trip is nothing against a 50 ms layer and fatal against an 8 us one.
     * ================================================================================== */
    const int N = 1000;
    double best = 1e9, sum = 0;
    for (int i = 0; i < N; i++) {
        uint8_t c;
        double a = now_s();
        if (write_all(fd, "P", 1) < 0) return 1;
        ssize_t r = read(fd, &c, 1);
        double d = now_s() - a;
        if (r != 1) { printf("ping lost at %d\n", i); break; }
        sum += d;
        if (d < best) best = d;
    }
    printf("round trip       %6.1f us mean, %6.1f us best, over %d pings\n",
           sum / N * 1e6, best * 1e6, N);

    /* =====================================================================================
     *  The verdict, computed rather than asserted.
     * ================================================================================== */
    double in_mbs = (double)total / t_in / 1048576.0;
    printf("\n--- verdict ---\n");
    printf("the Teensy reads its own PSRAM at 18.2 MB/s effective and DDR3 through the\n");
    printf("FPGA at 15.2. This link delivers %.2f MB/s inbound.\n", in_mbs);
    if (in_mbs > 20.0)
        printf("ABOVE both: the Luckfox can feed a Teensy faster than the Teensy can feed\n"
               "itself, so holding the model on the Luckfox is sound.\n");
    else if (in_mbs > 15.2)
        printf("BETWEEN them: the link beats the DDR3 path but not the PSRAM. Worth it only\n"
               "for data that does not fit in 8 MB.\n");
    else
        printf("BELOW both: forwarding a weight over this link is slower than the Teensy\n"
               "fetching it itself. The Luckfox is a bottleneck here, not a helper, and its\n"
               "value has to come from owning the protocol rather than from moving bytes.\n");

    close(fd);
    return 0;
}
