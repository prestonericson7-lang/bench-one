/* ===========================================================================================
 *  hdc_coord.c -- BENCH ONE: the coordinator. Turns N boxes into one memory.
 * ===========================================================================================
 *
 *  WHAT IT DOES
 *  -------------
 *  Broadcasts one hypervector to every node, collects 16-byte replies until a deadline, and
 *  merges them. That is the whole distributed algorithm. There is no leader election, no
 *  consensus, no membership protocol and no retry logic, because the merge is a monoid and
 *  none of those things are needed to get a correct answer out of it.
 *
 *  A query is 1032 bytes out, 24 bytes back per node. With eleven Luckfoxes that is one
 *  datagram out and 264 bytes in, for a search across ~440,000 concepts.
 *
 *
 *  THE DEADLINE IS THE DESIGN
 *  ---------------------------
 *  The coordinator NEVER waits for a specific node. It waits for a fixed wall-clock window and
 *  then answers with whatever arrived. A node that is swapping, rebooting, throttled, or
 *  physically unplugged is indistinguishable from one that is merely slow -- and it does not
 *  matter, because both cases produce the same thing: less coverage, and an honest number
 *  saying so.
 *
 *  Coverage is reported with every answer and it is the number that matters more than the
 *  match. "Nearest at distance 3900, coverage 34%" means the machine does not know. "Nearest
 *  at 3900, coverage 100%" means the machine has genuinely never seen this. Same match, very
 *  different statements, and only the coverage separates them.
 *
 *
 *  BUILD
 *      arm-rockchip830-linux-uclibcgnueabihf-gcc -O2 -o hdc_coord \
 *          hdc_coord.c bench_hdc.c bench_hdc_shard.c
 *
 *  RUN
 *      ./hdc_coord --peers 10.0.0.20,10.0.0.21,10.0.0.22 --self-test
 *      ./hdc_coord --peers ... --teach 500        # store 500 derived concepts, round robin
 *      ./hdc_coord --peers ... --bench 200        # 200 queries, report latency + coverage
 * ===========================================================================================
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>

/* ---- sleeping for a fraction of a second, without usleep -----------------------------------
 *
 * This file declares _POSIX_C_SOURCE 200809L, and POSIX.1-2008 **deleted** usleep. It had been
 * deprecated since 2001. So calling it here was not a portability nicety, it was a bug: the
 * declaration is hidden by the very macro at the top of this file, the compiler falls back to an
 * implicit declaration, and the program links only because the symbol happens to survive in libc.
 *
 * It built anyway because build.sh uses -Wall -Wextra without -Werror, so the warning scrolled past
 * on every build and nobody read it. On a target where the implicit `int` return convention differs
 * from the real one, that is how a silent miscompile starts.
 *
 * nanosleep is the POSIX.1-2008 replacement, is declared under this macro, and restarts correctly
 * when a signal interrupts it -- which usleep did not, and which matters here because the
 * coordinator installs a handler for orderly shutdown. */
static void sleep_us(long us)
{
    struct timespec ts;
    ts.tv_sec  = us / 1000000L;
    ts.tv_nsec = (us % 1000000L) * 1000L;
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
        /* nanosleep writes the remaining time back into ts, so this resumes rather than restarts. */
    }
}

#include "bench_hdc.h"
#include "bench_hdc_shard.h"
#include "bench_hdc_deep.h"

#define MSG_QUERY 0x51u
#define MSG_REPLY 0x52u
#define MSG_STORE 0x53u
#define MSG_DEEP  0x55u
#define MSG_DEEPREP 0x56u
#define QUERY_HDR 8
#define QUERY_BYTES (QUERY_HDR + HD_BYTES)
#define MAX_PEERS 64
#define FIRST_NODE 20      /* peer i is node FIRST_NODE+i -- must match build.sh */

static struct sockaddr_in g_peer[MAX_PEERS];
static int  g_npeers = 0;
static int  g_sock   = -1;
static int  g_port   = 9000;

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static uint64_t now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)(t.tv_nsec / 1000);
}

static void send_all(uint8_t type, uint16_t qid, uint16_t label,
                     uint16_t deadline_ms, const hd_t v, int only_peer)
{
    uint8_t pkt[QUERY_BYTES];
    pkt[0] = type; pkt[1] = 0;
    put16(pkt + 2, qid); put16(pkt + 4, label); put16(pkt + 6, deadline_ms);
    memcpy(pkt + QUERY_HDR, v, HD_BYTES);

    if (only_peer >= 0) {
        sendto(g_sock, pkt, sizeof(pkt), 0,
               (struct sockaddr *)&g_peer[only_peer], sizeof(g_peer[0]));
        return;
    }
    for (int i = 0; i < g_npeers; i++)
        sendto(g_sock, pkt, sizeof(pkt), 0,
               (struct sockaddr *)&g_peer[i], sizeof(g_peer[0]));
}

/* Collect replies until the deadline. Returns how many DISTINCT nodes were counted.
 *
 * Note what is missing: there is no list of who we are still waiting for, and no timeout per
 * peer. The merge dedups by node id, so a duplicate is free, and a node that never answers is
 * simply never counted. Tracking outstanding peers would add state that can go stale and buy
 * nothing the coverage number does not already say. */
/* ===========================================================================================
 * THE SHORTLIST -- what stage 2 is given to work with
 * -------------------------------------------------------------------------------------------
 * Stage 2 scores a list of candidates at full dimension. Something has to produce that list, and
 * stage 1 already does without being asked: every node reports its own local winner, so one
 * candidate per node falls out of the replies that are arriving anyway. No extra round trip, no
 * protocol change.
 *
 * That is also exactly the right list rather than a convenient one. Stage 2 exists for the case
 * where the GLOBAL stage-1 winner is wrong and the true answer was some other node's local winner
 * -- a concept whose 8192-bit slice looks unremarkable while its full 90,112-bit vector is the
 * closest thing in the cluster. Every such candidate is, by definition, some node's local best, so
 * one per node captures all of them.
 *
 * Per-node MAX, like hd_merge_add, so a retry replaces a node's earlier partial answer instead of
 * adding a second entry for the same node. Two entries for one node would be harmless for
 * correctness -- stage 2 scores each candidate independently -- but it would waste a slot out of
 * HD_MAX_K and shrink the shortlist for no reason.
 * =========================================================================================*/
typedef struct {
    uint16_t k;
    uint32_t cand[HD_MAX_K];
    uint32_t dist[HD_MAX_K];       /* the stage-1 distance, kept only to pick the per-node best */
    uint16_t node[HD_MAX_K];
} shortlist_t;

static void sl_begin(shortlist_t *sl) { sl->k = 0; }

static void sl_offer(shortlist_t *sl, const hd_partial_t *p)
{
    if (!sl) return;
    if (p->best_local == 0xFFFFu) return;                   /* the node scanned nothing */
    const uint32_t slot = (uint32_t)p->base + p->best_local;

    for (uint16_t i = 0; i < sl->k; i++) {
        if (sl->node[i] != p->node) continue;
        if (p->best_dist < sl->dist[i]) {                   /* a fuller retry beat its own earlier answer */
            sl->cand[i] = slot;
            sl->dist[i] = p->best_dist;
        }
        return;
    }
    if (sl->k >= HD_MAX_K) return;                          /* more nodes than the protocol carries */
    sl->cand[sl->k] = slot;
    sl->dist[sl->k] = p->best_dist;
    sl->node[sl->k] = p->node;
    sl->k++;
}

static int collect(hd_merge_t *m, uint16_t qid, uint32_t window_ms, shortlist_t *sl)
{
    const uint64_t deadline = now_us() + (uint64_t)window_ms * 1000u;
    int counted = 0;

    for (;;) {
        const uint64_t nowv = now_us();
        if (nowv >= deadline) break;

        struct timeval tv;
        const uint64_t left = deadline - nowv;
        tv.tv_sec  = (long)(left / 1000000u);
        tv.tv_usec = (long)(left % 1000000u);

        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(g_sock, &rd);
        const int r = select(g_sock + 1, &rd, NULL, NULL, &tv);
        if (r <= 0) { if (r < 0 && errno == EINTR) continue; break; }

        uint8_t buf[128];
        const ssize_t n = recvfrom(g_sock, buf, sizeof(buf), 0, NULL, NULL);
        if (n < (ssize_t)(QUERY_HDR + HD_PARTIAL_WIRE_BYTES)) continue;
        if (buf[0] != MSG_REPLY) continue;

        hd_partial_t p;
        if (!hd_partial_unpack(&p, buf + QUERY_HDR)) continue;   /* malformed: drop, not merge */
        if (p.query_id != qid) continue;                        /* late answer to an old ask  */

        sl_offer(sl, &p);
        counted += hd_merge_add(m, &p);
    }
    return counted;
}

/* ===========================================================================================
 * COVERAGE-DRIVEN RETRY
 * -------------------------------------------------------------------------------------------
 * One round answers ~92% of the memory, because a node preempted mid-scan reports what it got
 * and stops. Re-asking ONLY the nodes that are still incomplete lifts that to ~99% in about
 * 2.3 rounds, and costs a fraction of a full query because most nodes are already done.
 *
 * This is safe only because hd_merge_add keeps the BEST report per node: a retry replaces a
 * partial answer rather than adding to it, so coverage can never be inflated by asking twice.
 * Measured in simulation: recall 94.7% -> 98.7%.
 * =========================================================================================*/
static int query_cluster(hd_merge_t *m, const hd_t v, uint16_t qid,
                         uint32_t window_ms, uint32_t cov_target_pct, int max_rounds,
                         int *rounds_used, shortlist_t *sl)
{
    hd_merge_begin(m, qid);
    if (sl) sl_begin(sl);
    int counted = 0;

    for (int round = 0; round < max_rounds; round++) {
        int asked = 0;
        for (int i = 0; i < g_npeers; i++) {
            /* Peer index maps to node id by position -- the same rule build.sh uses to assign
             * them. Ask only nodes that have not yet reported their whole shard. */
            const uint16_t node = (uint16_t)(FIRST_NODE + i);
            if (round > 0 && !hd_merge_node_incomplete(m, node)) continue;
            send_all(MSG_QUERY, qid, 0, (uint16_t)window_ms, v, i);
            asked++;
        }
        if (rounds_used) (*rounds_used)++;
        if (!asked) break;

        counted += collect(m, qid, window_ms, sl);

        if (m->best.total &&
            (uint32_t)m->best.scanned * 100u >= (uint32_t)m->best.total * cov_target_pct)
            break;
    }
    return counted;
}

/* ===========================================================================================
 * STAGE 2 -- the whole cluster on one question, at full dimension
 * -------------------------------------------------------------------------------------------
 * Sends candidate IDs plus the query, collects one uint16 per candidate per node, sums them.
 * The sum IS the true distance at HD_BITS * nodes wide.
 *
 * Returns 1 only when every slice reported. A partial sum is smaller than a complete one, so
 * a candidate scored by nine nodes would beat one scored by eleven -- acting on an incomplete
 * deep result is the single way this design can be confidently wrong, and it is refused here
 * rather than reported with a caveat.
 * =========================================================================================*/
static int deep_pass(hd_deep_t *d, const hd_t v, uint16_t qid,
                     const uint32_t *cand, uint16_t k, uint32_t window_ms)
{
    hd_deep_begin(d, qid, cand, k, (uint16_t)g_npeers);

    uint8_t pkt[QUERY_HDR + HD_DEEP_REQ_BYTES(HD_MAX_K) + HD_BYTES];
    pkt[0] = MSG_DEEP; pkt[1] = 0;
    put16(pkt + 2, qid); put16(pkt + 4, 0); put16(pkt + 6, (uint16_t)window_ms);
    const uint32_t rb = hd_deep_pack_req(d, pkt + QUERY_HDR, sizeof(pkt) - QUERY_HDR);
    if (!rb) return 0;
    memcpy(pkt + QUERY_HDR + rb, v, HD_BYTES);
    const size_t plen = QUERY_HDR + rb + HD_BYTES;

    for (int i = 0; i < g_npeers; i++)
        sendto(g_sock, pkt, plen, 0, (struct sockaddr *)&g_peer[i], sizeof(g_peer[0]));

    const uint64_t deadline = now_us() + (uint64_t)window_ms * 1000u;
    static uint16_t part[HD_MAX_K];
    for (;;) {
        const uint64_t nv = now_us();
        if (nv >= deadline || hd_deep_ready(d)) break;
        struct timeval tv;
        const uint64_t left = deadline - nv;
        tv.tv_sec = (long)(left / 1000000u); tv.tv_usec = (long)(left % 1000000u);
        fd_set rd; FD_ZERO(&rd); FD_SET(g_sock, &rd);
        if (select(g_sock + 1, &rd, NULL, NULL, &tv) <= 0) break;

        uint8_t buf[QUERY_HDR + 6 + 2 * HD_MAX_K];
        const ssize_t n = recvfrom(g_sock, buf, sizeof(buf), 0, NULL, NULL);
        if (n < (ssize_t)(QUERY_HDR + 6) || buf[0] != MSG_DEEPREP) continue;
        uint16_t rq = 0, rn = 0, rk = 0;
        if (!hd_deep_unpack_rep(&rq, &rn, &rk, part, HD_MAX_K,
                                buf + QUERY_HDR, (uint32_t)(n - QUERY_HDR))) continue;
        hd_deep_add(d, rn, rq, part, rk);
    }
    return hd_deep_ready(d);
}

int main(int argc, char **argv)
{
    const char *peers = NULL;
    int self_test = 0, teach = 0, bench = 0, deep = 0;
    uint32_t window_ms = 120;

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--peers")  && i + 1 < argc) peers = argv[++i];
        else if (!strcmp(argv[i], "--port")   && i + 1 < argc) g_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--window") && i + 1 < argc) window_ms = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--teach")  && i + 1 < argc) teach = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bench")  && i + 1 < argc) bench = atoi(argv[++i]);
        /* Stage 2 is OPT-IN, and under the sharding this cluster actually uses it CANNOT CHANGE
         * THE ANSWER. Measured, not assumed: tests/deep_test.c claim 5, 300 queries, zero
         * differences.
         *
         * bench_hdc_deep.h was written for a BIT-sliced cluster, where every node holds a slice of
         * the bits of every concept and the true distance exists only as a sum. build.sh and
         * hdc_node.c build a ROW-sharded one instead: each node holds different whole concepts at
         * full width, with its own --base. hd_deep_score correctly gives a candidate the maximum
         * distance on any node that does not hold it, but every candidate is held by exactly one
         * node, so every candidate collects the same number of maxima. The sum is stage 1's
         * distance plus a constant, and a constant cannot reorder anything.
         *
         * That is why deep_pass() sat uncalled: it was written for a cluster shape this project
         * does not currently build. The flag is kept because the code is correct and the day this
         * memory is bit-sliced it becomes necessary, and because leaving a whole subsystem
         * unreferenced is how it rots. It prints what it is rather than implying a benefit. */
        else if (!strcmp(argv[i], "--deep")) deep = 1;
        else if (!strcmp(argv[i], "--self-test")) self_test = 1;
        else {
            fprintf(stderr,
                "usage: %s --peers ip,ip,... [--port N] [--window MS]\n"
                "          [--self-test] [--teach N] [--bench N]\n", argv[0]);
            return 1;
        }
    }
    if (!peers) { fprintf(stderr, "--peers is required\n"); return 1; }

    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", peers);
    for (char *t = strtok(tmp, ","); t && g_npeers < MAX_PEERS; t = strtok(NULL, ",")) {
        memset(&g_peer[g_npeers], 0, sizeof(g_peer[0]));
        g_peer[g_npeers].sin_family = AF_INET;
        g_peer[g_npeers].sin_port   = htons((uint16_t)g_port);
        if (inet_pton(AF_INET, t, &g_peer[g_npeers].sin_addr) != 1) {
            fprintf(stderr, "bad address: %s\n", t);
            return 1;
        }
        g_npeers++;
    }

    g_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_sock < 0) { perror("socket"); return 1; }

    printf("coordinator: %d peers, port %d, %u ms window\n",
           g_npeers, g_port, (unsigned)window_ms);

    uint32_t rng = 0xC0DE0001u;
    uint16_t qid = 0;

    /* --- teach: one-shot learning across the cluster ---------------------------------- */
    if (teach) {
        printf("\nteaching %d concepts, round-robin across %d nodes...\n", teach, g_npeers);
        for (int i = 0; i < teach; i++) {
            hd_t v;
            uint32_t s = 0xBEEF0000u + (uint32_t)i;
            hd_random(v, &s);                     /* derived, so any node can regenerate it */
            send_all(MSG_STORE, ++qid, (uint16_t)i, 0, v, i % g_npeers);
            if ((i % 64) == 63) sleep_us(2000);   /* let the receivers drain their sockets */
        }
        sleep_us(200000);
        printf("taught %d concepts (one shot each, no training loop)\n", teach);
    }

    /* --- self-test: ask for something we taught, and something we did not -------------- */
    if (self_test) {
        printf("\n--- recall of a taught concept -----------------------------------\n");
        for (int trial = 0; trial < 3; trial++) {
            hd_t v;
            uint32_t s = 0xBEEF0000u + (uint32_t)trial;
            hd_random(v, &s);

            /* Corrupt 20% of the bits. This is the whole point of the representation: a
             * fifth of the memory can be wrong and recall still lands on the right concept. */
            uint32_t nz = 0xA5A5u + trial;
            for (uint32_t b = 0; b < HD_BITS / 5u; b++) {
                const uint32_t bit = hd_rand(&nz) % HD_BITS;
                v[bit >> 5] ^= (1u << (bit & 31u));
            }

            hd_merge_t m;
            shortlist_t sl;
            int rounds = 0;
            const uint64_t t0 = now_us();
            const int got = query_cluster(&m, v, ++qid, window_ms, 97, 3, &rounds, &sl);
            const uint64_t dt = now_us() - t0;

            printf("  concept %d + 20%% noise -> slot %ld  dist %lu/%d  "
                   "%d/%d nodes  %llu us  dupes %u\n",
                   trial, (long)hd_partial_slot(&m.best),
                   (unsigned long)m.best.best_dist, HD_BITS,
                   got, g_npeers, (unsigned long long)dt, (unsigned)m.dupes);

            /* ---- stage 2, if asked for -----------------------------------------------------
             * Every node rescores the whole shortlist against its own slice and the partials are
             * summed, which makes the total the TRUE distance at HD_BITS * nodes wide. Stage 1
             * compared 8192-bit slices; this compares the full 90,112.
             *
             * hd_deep_ready() is checked rather than trusted: a missing slice makes every
             * candidate look closer, so a candidate scored by nine nodes would beat one scored by
             * eleven. A partial deep result is refused outright instead of reported with a
             * caveat -- it is the one way this design could be confidently wrong. */
            if (deep && sl.k) {
                if (trial == 0)
                    printf("      (--deep re-scores at full width. Under ROW sharding it cannot\n"
                           "       change the answer -- see tests/deep_test.c claim 5. It is\n"
                           "       here to exercise the path, not to improve recall.)\n");
                hd_deep_t d;
                const uint64_t d0 = now_us();
                const int complete = deep_pass(&d, v, ++qid, sl.cand, sl.k, window_ms);
                const uint64_t ddt = now_us() - d0;

                if (!complete) {
                    printf("      deep: REFUSED -- only some slices reported, so the sums are "
                           "not comparable (%u candidates, %llu us)\n",
                           (unsigned)sl.k, (unsigned long long)ddt);
                } else {
                    uint32_t dist = 0, margin_q8 = 0;
                    const int32_t win = hd_deep_best(&d, &dist, &margin_q8);
                    printf("      deep: slot %ld  dist %lu/%d  margin %.1f sigma  "
                           "%u candidates  %llu us%s\n",
                           (long)win, (unsigned long)dist, HD_BITS * g_npeers,
                           margin_q8 / 256.0, (unsigned)sl.k,
                           (unsigned long long)ddt,
                           (win == hd_partial_slot(&m.best)) ? "" : "   <-- CHANGED the answer");
                }
            }
        }

        printf("\n--- something never taught ---------------------------------------\n");
        hd_t v;
        hd_random(v, &rng);
        hd_merge_t m;
        hd_merge_begin(&m, ++qid);
        send_all(MSG_QUERY, qid, 0, (uint16_t)window_ms, v, -1);
        const int got = collect(&m, qid, window_ms, NULL);
        printf("  novel vector -> nearest dist %lu (orthogonal is %d)\n",
               (unsigned long)m.best.best_dist, HD_BITS / 2);
        printf("  scanned %u concepts across %d/%d nodes\n",
               (unsigned)m.best.scanned, got, g_npeers);
        printf("  %s\n", (m.best.best_dist > (HD_BITS * 2u) / 5u)
               ? "correctly reports: I have never seen this"
               : "WARNING: matched something it should not have");
    }

    /* --- bench: latency and coverage under whatever the cluster is actually doing ------ */
    if (bench) {
        printf("\n--- %d queries -------------------------------------------------\n", bench);
        uint64_t tot = 0, worst = 0;
        uint64_t cov_sum = 0;
        int full = 0;
        for (int i = 0; i < bench; i++) {
            hd_t v;
            hd_random(v, &rng);
            hd_merge_t m;
            int rounds = 0;
            const uint64_t t0 = now_us();
            const int got = query_cluster(&m, v, ++qid, window_ms, 97, 3, &rounds, NULL);
            const uint64_t dt = now_us() - t0;
            tot += dt;
            if (dt > worst) worst = dt;
            cov_sum += m.best.scanned;
            if (got == g_npeers) full++;
        }
        printf("  mean latency   : %llu us\n", (unsigned long long)(tot / (uint64_t)bench));
        printf("  worst latency  : %llu us\n", (unsigned long long)worst);
        printf("  mean scanned   : %llu concepts/query\n",
               (unsigned long long)(cov_sum / (uint64_t)bench));
        printf("  all nodes replied: %d/%d queries (%.1f%%)\n",
               full, bench, 100.0 * full / bench);
        printf("  the other %d were answered with partial coverage, not dropped.\n",
               bench - full);
    }

    close(g_sock);
    return 0;
}
