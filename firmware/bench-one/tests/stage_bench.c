/* ===========================================================================================
 *  stage_bench.c -- what does it actually cost to hand an activation to the next stage?
 * ===========================================================================================
 *
 *  Prediction 3 in docs/21 says a 16 KB activation crosses gigabit in 0.13 to 0.20 ms one way,
 *  including stack overhead, and that if it comes out above 1 ms the network becomes the bottleneck
 *  and the design needs 10 Gbit. This measures it.
 *
 *  THREE PLACES TO RUN IT, AND THEY MEASURE DIFFERENT THINGS
 *  ---------------------------------------------------------
 *      loopback   both ends on one machine. Measures the PROTOCOL and the kernel's socket path
 *                 with no wire involved, which is the floor: no real link can beat it. Anything
 *                 above this figure on real hardware is the network, and anything at it is the
 *                 software.
 *      USB        two Luckfoxes routed through the PC's USB networking. Free, works today.
 *      ethernet   the real thing, once magjacks exist.
 *
 *  Running loopback first is not a warm-up. Without it, a slow result on real hardware cannot be
 *  attributed: it could be the wire, the driver, or this code being careless with syscalls. The
 *  floor separates those.
 *
 *  WHAT IS REPORTED AND WHY IT IS THE MEDIAN AND THE TAIL, NOT THE MEAN
 *  ---------------------------------------------------------------------
 *  Pipeline throughput is set by the slowest stage, and time to first token is the sum of every
 *  stage. So the tail matters as much as the middle: one hop in a hundred taking 20 ms would be
 *  invisible in a mean and would dominate a 32-stage pipeline. p99 and worst are printed for that
 *  reason.
 *
 *  BUILD
 *      gcc -O2 -std=gnu11 -I../shared -o stage_bench.exe stage_bench.c ../shared/stage_link.c -lws2_32
 *      arm-none-linux-gnueabihf-gcc -O2 -static -std=gnu11 -I../shared \
 *          -o stage_bench stage_bench.c ../shared/stage_link.c
 *
 *  RUN
 *      stage_bench serve 9100                 on the far node
 *      stage_bench probe 127.0.0.1 9100       on the near one
 *      stage_bench both  9100                 loopback, both ends in one process
 * ===========================================================================================
 */

#include "stage_link.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <process.h>
  #define THREAD_RET unsigned __stdcall
#else
  #include <pthread.h>
  #define THREAD_RET void *
#endif

static uint8_t g_buf[STAGE_MAX_PAYLOAD];
static uint8_t g_rx[STAGE_MAX_PAYLOAD];

/* --- the far end: receive an activation, acknowledge it, repeat ----------------------------- */

static int serve(uint16_t port, int forever)
{
    stage_fd_t srv = stage_listen(port);
    if (srv == STAGE_BAD_FD) { printf("  cannot listen on %u\n", port); return 1; }
    printf("  listening on %u\n", port);
    fflush(stdout);

    do {
        stage_fd_t c = stage_accept(srv);
        if (c == STAGE_BAD_FD) continue;
        for (;;) {
            stage_hdr_t h;
            if (stage_recv(c, &h, g_rx, sizeof(g_rx)) != 0) break;
            if (h.type == STAGE_BYE) { stage_close(c); goto done; }

            /* The acknowledgement carries t_emit_us back UNCHANGED. That is what lets the sender
             * time a round trip against its own clock alone, with no synchronisation between the
             * two machines and none claimed. */
            stage_hdr_t a;
            memset(&a, 0, sizeof(a));
            a.type = STAGE_ACK;
            a.token_id = h.token_id;
            a.stage = h.stage;
            a.seq = h.seq;
            a.t_emit_us = h.t_emit_us;
            a.len = 0;
            if (stage_send(c, &a, NULL) != 0) break;
        }
        stage_close(c);
    } while (forever);
done:
    stage_close(srv);
    return 0;
}

/* --- the near end: time round trips at several payload sizes -------------------------------- */

static int cmp_u64(const void *a, const void *b)
{
    const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static int probe(const char *host, uint16_t port)
{
    stage_fd_t s = stage_connect(host, port);
    if (s == STAGE_BAD_FD) { printf("  cannot connect to %s:%u\n", host, port); return 1; }

    const uint32_t sizes[] = { 64, 4096, 16384, 65536, 262144 };
    const int nsize = (int)(sizeof(sizes) / sizeof(sizes[0]));
    const int iters = 300;
    uint64_t *rtt = malloc((size_t)iters * sizeof(uint64_t));
    if (!rtt) return 1;

    for (uint32_t i = 0; i < sizeof(g_buf); i++) g_buf[i] = (uint8_t)i;

    printf("\n  payload     median      p99       worst     one-way*    MB/s\n");
    printf("  --------  ---------  ---------  ---------  ---------  --------\n");

    uint32_t seq = 0;
    for (int si = 0; si < nsize; si++) {
        const uint32_t n = sizes[si];

        /* Warm the path before timing: the first exchange on a connection pays for ARP, route
         * lookup and page faults that no later one repeats, and including it would slander the
         * link with a cost it only charges once. */
        for (int w = 0; w < 5; w++) {
            stage_hdr_t h;
            memset(&h, 0, sizeof(h));
            h.type = STAGE_ACT; h.len = n; h.seq = seq++; h.t_emit_us = stage_now_us();
            stage_hdr_t a;
            if (stage_send(s, &h, g_buf) != 0) goto fail;
            if (stage_recv(s, &a, NULL, 0) != 0) goto fail;
        }

        for (int k = 0; k < iters; k++) {
            stage_hdr_t h, a;
            memset(&h, 0, sizeof(h));
            h.type = STAGE_ACT;
            h.token_id = (uint32_t)k;
            h.len = n;
            h.seq = seq++;
            h.t_emit_us = stage_now_us();
            if (stage_send(s, &h, g_buf) != 0) goto fail;
            if (stage_recv(s, &a, NULL, 0) != 0) goto fail;
            const uint64_t now = stage_now_us();

            /* The echo must match what was sent, or the number being measured is not a round trip
             * of this message. Cheap to check and it catches a peer that reorders or invents. */
            if (a.t_emit_us != h.t_emit_us || a.seq != h.seq) {
                printf("  echo mismatch at seq %u -- the far end is not replying in order\n", h.seq);
                goto fail;
            }
            rtt[k] = now - a.t_emit_us;
        }

        qsort(rtt, (size_t)iters, sizeof(uint64_t), cmp_u64);
        const double med = (double)rtt[iters / 2];
        const double p99 = (double)rtt[(int)(iters * 0.99)];
        const double wst = (double)rtt[iters - 1];
        printf("  %7u  %8.1fus %8.1fus %8.1fus %8.1fus %8.1f\n",
               n, med, p99, wst, med / 2.0, (double)n / med);
    }

    printf("\n  * one-way is half the round trip and assumes a symmetric path. Over USB\n");
    printf("    networking that assumption is worth doubting.\n");

    {
        stage_hdr_t b;
        memset(&b, 0, sizeof(b));
        b.type = STAGE_BYE;
        stage_send(s, &b, NULL);
    }
    stage_close(s);
    free(rtt);
    return 0;
fail:
    printf("  link failed part way through\n");
    stage_close(s);
    free(rtt);
    return 1;
}

/* --- loopback: both ends in one process, to establish the floor ----------------------------- */

static uint16_t g_port;

static THREAD_RET server_thread(void *arg)
{
    (void)arg;
    serve(g_port, 0);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

int main(int argc, char **argv)
{
    if (stage_init() != 0) { printf("socket startup failed\n"); return 1; }

    printf("\n===============================================================\n");
    printf("stage link: what one hop between pipeline stages costs\n");
    printf("===============================================================\n");

    int rc = 1;
    if (argc >= 3 && !strcmp(argv[1], "serve")) {
        rc = serve((uint16_t)atoi(argv[2]), 1);
    } else if (argc >= 4 && !strcmp(argv[1], "probe")) {
        rc = probe(argv[2], (uint16_t)atoi(argv[3]));
    } else if (argc >= 3 && !strcmp(argv[1], "both")) {
        g_port = (uint16_t)atoi(argv[2]);
        printf("  loopback: this is the FLOOR. No real link beats it, so a slower\n");
        printf("  figure on hardware is the wire and a matching one is this code.\n");
#ifdef _WIN32
        _beginthreadex(NULL, 0, server_thread, NULL, 0, NULL);
        Sleep(400);
#else
        pthread_t t;
        pthread_create(&t, NULL, server_thread, NULL);
        struct timespec ts = { 0, 400000000L };
        nanosleep(&ts, NULL);
#endif
        rc = probe("127.0.0.1", g_port);
    } else {
        printf("\n  stage_bench serve <port>            far node\n");
        printf("  stage_bench probe <host> <port>     near node\n");
        printf("  stage_bench both  <port>            loopback, the floor\n\n");
    }

    stage_shutdown();
    return rc;
}
