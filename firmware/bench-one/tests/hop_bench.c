/* ===========================================================================================
 *  hop_bench.c -- what a hop costs between two real machines, not between a machine and itself
 * ===========================================================================================
 *
 *  stage_bench measured 29 us for a 64-byte round trip and 32 us for 16 KB, on loopback. That number
 *  is load-bearing: every claim that distributing this machine costs under 1% rests on it, and it was
 *  measured on a socket that never left the process.
 *
 *  This measures the same thing between this PC and a Luckfox over USB Ethernet. Two separate
 *  processors, two separate memories, a real NIC driver at each end, a real cable.
 *
 *
 *  THE CLIENT DOES THE TIMING, AND THE CLIENT IS THE BOARD
 *  -------------------------------------------------------
 *  Both ends of a round trip are timed on ONE clock, so nothing has to be synchronised and there is no
 *  skew to argue about. The board is the client because on this image the board can open a connection
 *  outward while the PC cannot reach in: Windows has three interfaces on 169.254.0.0/16 and sends all
 *  link-local traffic out the one with the lowest metric, which here is a VPN tunnel. Rather than
 *  rearrange the PC's networking, the direction that already works is the direction used.
 *
 *
 *  WHAT THE SIZES MEAN
 *  --------------------
 *  64 B      the floor. Pure per-message cost: syscalls, interrupts, driver, wire.
 *  8 KB      one activation of Qwen2.5-Coder 3B, which is what actually crosses between stages.
 *  32 KB     four activations, i.e. what a small prefill batch looks like.
 *  196 KB    a 24-position prefill batch, the largest message the pipeline sends.
 *
 *  If the 8 KB figure is a small fraction of a stage's compute time, pipeline parallelism survives on
 *  this link and the machine can be wired with USB cables instead of ethernet adapters. If it is not,
 *  that is worth knowing before buying anything.
 *
 *  RUN
 *      on the PC     hop_bench server [port]
 *      on the board  hop_bench client <pc-ip> [port]
 * ===========================================================================================
 */

#include "stage_link.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEF_PORT 9400

static const uint32_t SIZES[] = { 64u, 8192u, 32768u, 196608u };
#define N_SIZES (sizeof(SIZES) / sizeof(SIZES[0]))
#define REPS    200

static uint8_t buf[262144];

/* ---------------------------------------------------------------------------------------------
 *  server: echo whatever arrives, as fast as possible
 * ------------------------------------------------------------------------------------------ */

static int run_server(uint16_t port)
{
    stage_fd_t srv = stage_listen(port);
    if (srv == STAGE_BAD_FD) { printf("cannot listen on %u\n", port); return 1; }
    printf("\n  echo server on port %u, waiting\n", port);
    fflush(stdout);

    stage_fd_t c = stage_accept(srv);
    if (c == STAGE_BAD_FD) { printf("  accept failed\n"); return 1; }
    printf("  client connected, echoing\n");
    fflush(stdout);

    uint64_t n = 0;
    for (;;) {
        stage_hdr_t h;
        if (stage_recv(c, &h, buf, sizeof(buf)) != 0) break;
        if (h.type == STAGE_BYE) break;
        /* Echoed back untouched, including t_emit_us, so the client can time the whole trip on its
         * own clock without either side knowing the other's. */
        if (stage_send(c, &h, buf) != 0) break;
        n++;
    }
    printf("  echoed %llu messages, client gone\n\n", (unsigned long long)n);
    stage_close(c);
    stage_close(srv);
    return 0;
}

/* ---------------------------------------------------------------------------------------------
 *  client: timed ping-pong
 * ------------------------------------------------------------------------------------------ */

static int cmp_u64(const void *a, const void *b)
{
    const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static int run_client(const char *host, uint16_t port)
{
    stage_fd_t fd = STAGE_BAD_FD;
    for (int t = 0; t < 100 && fd == STAGE_BAD_FD; t++) fd = stage_connect(host, port);
    if (fd == STAGE_BAD_FD) { printf("cannot reach %s:%u\n", host, port); return 1; }

    printf("\n======================================================================\n");
    printf("  one hop, machine to machine: this board to %s\n", host);
    printf("======================================================================\n");
    printf("  %8s %10s %10s %10s %10s %12s\n",
           "bytes", "median", "p10", "p90", "worst", "MB/s");

    uint64_t *t = (uint64_t *)malloc(REPS * sizeof(uint64_t));
    if (!t) { printf("out of memory\n"); return 1; }

    for (size_t si = 0; si < N_SIZES; si++) {
        const uint32_t sz = SIZES[si];
        memset(buf, (int)(si + 1), sz);

        /* A few untimed messages first: the first packet on a fresh connection pays for ARP, a cold
         * driver path and TCP's initial window, and reporting that as the hop cost would be wrong. */
        for (int w = 0; w < 20; w++) {
            stage_hdr_t h;
            memset(&h, 0, sizeof(h));
            h.type = STAGE_ACT;
            h.len = sz;
            if (stage_send(fd, &h, buf) != 0) { printf("  send failed\n"); return 1; }
            if (stage_recv(fd, &h, buf, sizeof(buf)) != 0) { printf("  recv failed\n"); return 1; }
        }

        for (int r = 0; r < REPS; r++) {
            stage_hdr_t h;
            memset(&h, 0, sizeof(h));
            h.type = STAGE_ACT;
            h.len = sz;
            h.seq = (uint32_t)r;
            const uint64_t t0 = stage_now_us();
            h.t_emit_us = t0;
            if (stage_send(fd, &h, buf) != 0) { printf("  send failed\n"); return 1; }
            if (stage_recv(fd, &h, buf, sizeof(buf)) != 0) { printf("  recv failed\n"); return 1; }
            t[r] = stage_now_us() - t0;
        }

        qsort(t, REPS, sizeof(uint64_t), cmp_u64);
        const uint64_t med = t[REPS / 2], p10 = t[REPS / 10], p90 = t[(REPS * 9) / 10];
        const uint64_t worst = t[REPS - 1];
        /* Bytes in both directions over the median round trip: the payload goes out and comes back. */
        const double mbs = med ? (2.0 * sz) / (double)med : 0.0;
        printf("  %8u %9lluus %9lluus %9lluus %9lluus %12.1f\n",
               sz, (unsigned long long)med, (unsigned long long)p10,
               (unsigned long long)p90, (unsigned long long)worst, mbs);
        fflush(stdout);
    }

    {
        stage_hdr_t b;
        memset(&b, 0, sizeof(b));
        b.type = STAGE_BYE;
        stage_send(fd, &b, NULL);
    }
    free(t);
    stage_close(fd);

    printf("\n  Halve the median for one-way. Compare the 8192-byte row against a stage's compute:\n");
    printf("  on this hardware a stage takes 100-180 ms, so anything under a millisecond here means\n");
    printf("  the interconnect is free and the machine can be wired with what is already on the desk.\n\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (stage_init() != 0) { printf("socket startup failed\n"); return 1; }
    int rc = 1;

    if (argc >= 2 && !strcmp(argv[1], "server")) {
        rc = run_server((uint16_t)(argc > 2 ? atoi(argv[2]) : DEF_PORT));
    } else if (argc >= 3 && !strcmp(argv[1], "client")) {
        rc = run_client(argv[2], (uint16_t)(argc > 3 ? atoi(argv[3]) : DEF_PORT));
    } else {
        printf("\n  hop_bench server [port]\n  hop_bench client <host> [port]\n\n");
    }
    stage_shutdown();
    return rc;
}
