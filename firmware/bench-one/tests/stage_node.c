/* ===========================================================================================
 *  stage_node.c -- one stage of a pipeline-parallel model, end to end
 * ===========================================================================================
 *
 *  This is demonstration 7 from the brief: a workload that exceeds the memory of any single node.
 *  Each node holds a contiguous run of layers and nothing else. No node can produce a token alone,
 *  and together they can, which is the entire claim the architecture rests on.
 *
 *  WHAT A STAGE DOES
 *  ------------------
 *      receive an activation from upstream
 *      for each layer it owns:  y = W . x,  requantise, x = y
 *      hand the result downstream
 *
 *  The last stage sends the result back to the head, which closes the loop and lets the head time
 *  a whole token against its own clock. No synchronisation between nodes is needed or claimed.
 *
 *  WHY THE WEIGHTS ARE GENERATED, NOT LOADED
 *  ------------------------------------------
 *  Real weights are not the point here and would only obscure it. What is being measured is whether
 *  the machine can hold a model larger than any node and move a token through it at a rate worth
 *  having. Deterministic pseudo-random weights let every node fill its memory to whatever size is
 *  asked for, and let the head verify the answer is reproducible, without needing a model file on
 *  every board.
 *
 *  The arithmetic is the real arithmetic: the same gemv_int4 the FPGA implements and the same one
 *  measured at 2.18 G MAC/s on a host. Only the numbers are synthetic.
 *
 *  WHAT IT REPORTS
 *  ----------------
 *  Time per token split into COMPUTE and LINK. That split is the whole experiment. If compute
 *  dominates, adding nodes helps and the architecture scales. If the link dominates, it does not,
 *  and no amount of extra hardware fixes it. Demonstration 8 is exactly this number.
 *
 *  RUN
 *      stage_node head <next-host> <port> <layers> <dim> <tokens>
 *      stage_node mid  <listen-port> <next-host> <next-port> <layers> <dim>
 *      stage_node tail <listen-port> <head-host> <head-port> <layers> <dim>
 *
 *  A two-node ring is head + tail. Three or more inserts mids between them.
 * ===========================================================================================
 */

#include "stage_link.h"
#include "bench_gemv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HEAD_PORT_DEFAULT 9200

static uint8_t  *g_w;        /* INT4 weights, packed two per byte, for every layer this node holds */
static int8_t   *g_x;        /* activation in                                                      */
static int8_t   *g_y;        /* activation out                                                     */
static int32_t  *g_acc;

static uint32_t g_layers, g_dim;

/* Deterministic weights, so every node fills real memory and the result is reproducible without
 * shipping a model file to each board. */
static void make_weights(uint32_t seed)
{
    const size_t per_layer = (size_t)g_dim * g_dim / 2;
    const size_t total = per_layer * g_layers;
    g_w = malloc(total);
    if (!g_w) { printf("  cannot allocate %.1f MB of weights\n", total / 1048576.0); exit(1); }

    uint32_t s = seed ? seed : 1u;
    for (size_t i = 0; i < total; i++) {
        s = s * 1103515245u + 12345u;
        /* Two nibbles per byte, each in -7..7 so the packing is lossless and the sums stay small. */
        const uint8_t lo = (uint8_t)(((s >> 16) % 15u) - 7u) & 0x0Fu;
        const uint8_t hi = (uint8_t)(((s >> 20) % 15u) - 7u) & 0x0Fu;
        g_w[i] = (uint8_t)(lo | (hi << 4));
    }
    printf("  holding %u layers of %ux%u INT4 = %.1f MB\n",
           g_layers, g_dim, g_dim, total / 1048576.0);
}

/* Run every layer this node owns. The requantise is a shift, not a scale factor: the point is to
 * spend the same arithmetic a real layer would, not to reproduce a specific model's numerics. */
static void run_layers(void)
{
    const size_t per_layer = (size_t)g_dim * g_dim / 2;
    for (uint32_t L = 0; L < g_layers; L++) {
        gemv_int4(g_w + per_layer * L, g_x, g_acc, g_dim, g_dim);
        for (uint32_t r = 0; r < g_dim; r++) {
            int32_t v = g_acc[r] >> 8;
            if (v > 127) v = 127;
            if (v < -128) v = -128;
            g_y[r] = (int8_t)v;
        }
        memcpy(g_x, g_y, g_dim);
    }
}

static void alloc_vectors(void)
{
    g_x = malloc(g_dim);
    g_y = malloc(g_dim);
    g_acc = malloc((size_t)g_dim * sizeof(int32_t));
    if (!g_x || !g_y || !g_acc) { printf("  out of memory\n"); exit(1); }
}

/* --- a middle or tail stage: receive, compute, forward --------------------------------------- */

static int serve_stage(uint16_t listen_port, const char *next_host, uint16_t next_port)
{
    stage_fd_t srv = stage_listen(listen_port);
    if (srv == STAGE_BAD_FD) { printf("  cannot listen on %u\n", listen_port); return 1; }
    printf("  listening on %u, forwarding to %s:%u\n", listen_port, next_host, next_port);
    fflush(stdout);

    stage_fd_t up = stage_accept(srv);
    if (up == STAGE_BAD_FD) return 1;
    printf("  upstream connected\n");
    fflush(stdout);

    /* Connect downstream only after upstream arrives, so a ring comes up in order and a node
     * cannot sit retrying against a peer that has not started yet. */
    stage_fd_t dn = STAGE_BAD_FD;
    for (int try = 0; try < 200 && dn == STAGE_BAD_FD; try++) dn = stage_connect(next_host, next_port);
    if (dn == STAGE_BAD_FD) { printf("  cannot reach %s:%u\n", next_host, next_port); return 1; }
    printf("  downstream connected, running\n");
    fflush(stdout);

    uint64_t compute_us = 0, n = 0;
    for (;;) {
        stage_hdr_t h;
        if (stage_recv(up, &h, g_x, g_dim) != 0) break;
        if (h.type == STAGE_BYE) { stage_send(dn, &h, NULL); break; }

        const uint64_t t0 = stage_now_us();
        run_layers();
        compute_us += stage_now_us() - t0;
        n++;

        h.len = g_dim;
        h.stage = (uint16_t)(h.stage + 1);
        h.layer = (uint16_t)(h.layer + g_layers);
        if (stage_send(dn, &h, g_x) != 0) break;
    }
    if (n) printf("  %llu tokens, %.2f ms compute each\n",
                  (unsigned long long)n, (double)compute_us / n / 1000.0);
    stage_close(up); stage_close(dn); stage_close(srv);
    return 0;
}

/* --- the head: emit a token, wait for it to come back around --------------------------------- */

static int run_head(const char *next_host, uint16_t next_port, uint32_t tokens)
{
    stage_fd_t back = stage_listen(HEAD_PORT_DEFAULT);
    if (back == STAGE_BAD_FD) { printf("  cannot listen on %u\n", HEAD_PORT_DEFAULT); return 1; }

    stage_fd_t dn = STAGE_BAD_FD;
    printf("  connecting to %s:%u\n", next_host, next_port);
    for (int try = 0; try < 200 && dn == STAGE_BAD_FD; try++) dn = stage_connect(next_host, next_port);
    if (dn == STAGE_BAD_FD) { printf("  cannot reach the next stage\n"); return 1; }

    printf("  waiting for the ring to close on %u\n", HEAD_PORT_DEFAULT);
    fflush(stdout);
    stage_fd_t up = stage_accept(back);
    if (up == STAGE_BAD_FD) return 1;
    printf("  ring closed, running %u tokens\n\n", tokens);

    uint64_t compute_us = 0, total_us = 0;
    uint64_t best = ~0ull, worst = 0;

    for (uint32_t t = 0; t < tokens; t++) {
        for (uint32_t i = 0; i < g_dim; i++) g_x[i] = (int8_t)((t + i) & 0x3F);

        const uint64_t t0 = stage_now_us();
        const uint64_t c0 = stage_now_us();
        run_layers();
        compute_us += stage_now_us() - c0;

        stage_hdr_t h;
        memset(&h, 0, sizeof(h));
        h.type = STAGE_ACT;
        h.token_id = t;
        h.len = g_dim;
        h.layer = (uint16_t)g_layers;
        h.seq = t;
        h.t_emit_us = t0;
        if (stage_send(dn, &h, g_x) != 0) { printf("  send failed\n"); break; }

        stage_hdr_t r;
        if (stage_recv(up, &r, g_y, g_dim) != 0) { printf("  ring broke\n"); break; }
        const uint64_t dt = stage_now_us() - t0;
        total_us += dt;
        if (dt < best) best = dt;
        if (dt > worst) worst = dt;
    }

    {
        stage_hdr_t b;
        memset(&b, 0, sizeof(b));
        b.type = STAGE_BYE;
        stage_send(dn, &b, NULL);
    }

    const double per = (double)total_us / tokens / 1000.0;
    const double comp = (double)compute_us / tokens / 1000.0;
    printf("  per token   : %.2f ms   -> %.2f tokens/sec\n", per, 1000.0 / per);
    printf("    this node's compute : %.2f ms\n", comp);
    printf("    everything else     : %.2f ms  (other stages plus every hop)\n", per - comp);
    printf("    best %.2f ms, worst %.2f ms\n", best / 1000.0, worst / 1000.0);
    printf("\n  If 'everything else' is dominated by the other stages' compute, adding nodes\n");
    printf("  helps and this scales. If it is dominated by the hops, it does not.\n");

    stage_close(dn); stage_close(up); stage_close(back);
    return 0;
}

int main(int argc, char **argv)
{
    if (stage_init() != 0) { printf("socket startup failed\n"); return 1; }
    printf("\n===============================================================\n");
    printf("pipeline stage: a model larger than any one node\n");
    printf("===============================================================\n");

    int rc = 1;
    if (argc >= 7 && !strcmp(argv[1], "head")) {
        g_layers = (uint32_t)atoi(argv[4]);
        g_dim = (uint32_t)atoi(argv[5]);
        alloc_vectors();
        make_weights(0x1000u);
        rc = run_head(argv[2], (uint16_t)atoi(argv[3]), (uint32_t)atoi(argv[6]));
    } else if (argc >= 7 && (!strcmp(argv[1], "mid") || !strcmp(argv[1], "tail"))) {
        g_layers = (uint32_t)atoi(argv[5]);
        g_dim = (uint32_t)atoi(argv[6]);
        alloc_vectors();
        make_weights(0x2000u + (uint32_t)atoi(argv[2]));
        rc = serve_stage((uint16_t)atoi(argv[2]), argv[3], (uint16_t)atoi(argv[4]));
    } else {
        printf("\n  stage_node head <next-host> <next-port> <layers> <dim> <tokens>\n");
        printf("  stage_node mid  <listen> <next-host> <next-port> <layers> <dim>\n");
        printf("  stage_node tail <listen> <head-host> %d <layers> <dim>\n\n", HEAD_PORT_DEFAULT);
        printf("  A two-node ring is head + tail. Insert mids for more.\n\n");
    }
    stage_shutdown();
    return rc;
}
