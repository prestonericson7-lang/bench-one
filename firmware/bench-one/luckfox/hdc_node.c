/* ===========================================================================================
 *  hdc_node.c -- BENCH ONE: the Luckfox node. Eleven of these are the machine's memory.
 * ===========================================================================================
 *
 *  WHY THIS IS THE BIGGEST PIECE
 *  ------------------------------
 *      11 Luckfox x ~40 MB usable   = ~440,000 concepts
 *       9 Teensy  x 16 MB PSRAM     = ~144,000
 *       6 ESP32-S3 x 8 MB PSRAM     =  ~48,000
 *       2 FPGA    x 1 GB DDR3       = ~2,000,000  (when they land)
 *
 *  The Luckfoxes hold the working memory. Everything else is either faster and smaller
 *  (Teensy DTCM) or not here yet (FPGA). So this daemon is what the machine mostly IS.
 *
 *
 *  WRITTEN IN PLAIN C FOR A REASON
 *  --------------------------------
 *  The Luckfox SDK toolchain is arm-rockchip830-linux-uclibcgnueabihf -- uClibc, not glibc.
 *  numpy cannot be built for it (Buildroot's numpy hard-depends on glibc or musl) and no pip
 *  wheel exists for a uClibc interpreter. Anyone who plans on Python numerics here discovers
 *  that after the hardware is assembled. So: C, libc only, no threads, no dependencies.
 *
 *
 *  THE SAME INVARIANT AS EVERY OTHER NODE
 *  ---------------------------------------
 *  Slow makes the answer later. Stalled or dead makes it less complete. Nothing makes it
 *  wrong. A Luckfox is preemptively scheduled so it does not suffer the S3's radio stalls,
 *  but it can still be swapping, throttled, or rebooting -- and the coordinator cannot tell
 *  those apart from a slow scan, which is exactly why it does not have to.
 *
 *
 *  BUILD
 *      arm-rockchip830-linux-uclibcgnueabihf-gcc -O2 -o hdc_node \
 *          hdc_node.c bench_hdc.c bench_hdc_shard.c bench_policy.c
 *
 *  RUN
 *      ./hdc_node --node 20 --shard 40000 --port 9000            # a memory node
 *      ./hdc_node --node 20 --coordinator --peers 21,22,23 ...   # also runs queries
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
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>

#include "bench_hdc.h"
#include "bench_hdc_shard.h"
#include "bench_hdc_deep.h"
#include "bench_dream.h"
#include "bench_index.h"
#include "psram_bank.h"

/* ===========================================================================================
 * WIRE PROTOCOL
 * -------------------------------------------------------------------------------------------
 * Two message types, both fixed size, both little-endian byte-by-byte. UDP on purpose: a lost
 * query costs one retry and a lost reply costs coverage, and neither is worth a TCP handshake
 * or a connection table on a 64 MB box holding eleven peers.
 * =========================================================================================*/
#define MSG_QUERY   0x51u      /* 'Q' -- coordinator -> nodes: here is a hypervector          */
#define MSG_REPLY   0x52u      /* 'R' -- node -> coordinator: 16-byte hd_partial_t            */
#define MSG_STORE   0x53u      /* 'S' -- coordinator -> node: remember this                   */
#define MSG_STAT    0x54u      /* 'T' -- ask a node how it is                                 */
#define MSG_DEEP    0x55u      /* 'U' -- score a shortlist against MY slice                    */
#define MSG_DEEPREP 0x56u      /* 'V' -- ...here are the partial distances                     */

/* Reply flag: this match came from a PROTOTYPE (a concept the node formed itself) rather than
 * a stored instance. The coordinator needs to know, because a prototype hit means "I know what
 * KIND of thing this is" and an instance hit means "I have seen this exact thing". */
#define HDC_FLAG_PROTOTYPE 0x01u

#define QUERY_HDR   8
#define QUERY_BYTES (QUERY_HDR + HD_BYTES)      /* 8 + 1024 = 1032, one UDP datagram          */

typedef struct {
    uint8_t  type;
    uint8_t  flags;
    uint16_t query_id;
    uint16_t label;        /* for MSG_STORE                                                   */
    uint16_t deadline_ms;
} msg_hdr_t;

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }

static void hdr_pack(const msg_hdr_t *h, uint8_t *o)
{
    o[0] = h->type; o[1] = h->flags;
    put16(o + 2, h->query_id); put16(o + 4, h->label); put16(o + 6, h->deadline_ms);
}
static void hdr_unpack(msg_hdr_t *h, const uint8_t *i)
{
    h->type = i[0]; h->flags = i[1];
    h->query_id = get16(i + 2); h->label = get16(i + 4); h->deadline_ms = get16(i + 6);
}

/* ===========================================================================================
 * STATE
 * =========================================================================================*/
static hd_t     *g_store;
static uint16_t *g_label;
static hd_mem_t  g_mem;          /* episodic: things that happened                            */

/* -------------------------------------------------------------------------------------------
 * THE DREAM LAYER
 * -----------------------------------------------------------------------------------------
 * Consolidation runs when the node has nothing else to do. That is not a scheduling
 * convenience -- it is the mechanism. A machine that only ever answers queries accumulates
 * instances forever and never notices that two hundred of them were the same kind of thing.
 * Running it on idle means the cluster gets better at generalising the longer it is left on,
 * which is the closest thing to intuition that can be built out of what is on the bench.
 *
 * Measured: 240 noisy experiences of 12 categories the machine was never told about produced
 * exactly 12 prototypes. On cases it had never seen, prototypes named the right category 100%
 * of the time and the raw instances 0%.
 * ----------------------------------------------------------------------------------------- */
static hd_t     *g_pstore;
static uint16_t *g_plabel;
static hd_mem_t  g_proto;
static uint16_t *g_support;
static uint8_t  *g_absorbed;
static hd_acc_t *g_work;
static bench_dream_t g_dream;
static int       g_dreaming = 0;     /* --dream                                               */

/* -------------------------------------------------------------------------------------------
 * THE DEEP TIER
 * -----------------------------------------------------------------------------------------
 * With --psram N this node keeps its INDEX in DDR2 (fast, scanned every query) and its BODIES
 * on the PSRAM stick (slow per byte, but a body is fetched roughly once per query so only the
 * latency matters -- 24 us against a microSD's 1 ms).
 *
 * That split is the whole reason the stick is worth building. Putting the INDEX on PSRAM would
 * make this node 24-60x slower than one without it, because the index is read in full on every
 * single query. Bodies are read one at a time.
 * ----------------------------------------------------------------------------------------- */
static psram_bank_t   g_ps;
static int            g_have_psram = 0;
static int            g_psram_chips = 0;
static hd_entry_t    *g_ientry = NULL;
static uint16_t      *g_ilabel = NULL;
static hd_index_t     g_index;
static int            g_use_index = 0;
static uint64_t       g_body_next = 0;      /* bump allocator into the stick */
static uint16_t  g_dcursor  = 0;
static uint64_t  g_dpasses  = 0;
static uint16_t  g_node   = 20;
static uint16_t  g_base   = 0;

/* WHICH SLICE OF THE CONCEPT THIS NODE HOLDS.
 *
 * Two layouts share this daemon, and the difference is interpretation, not storage -- either
 * way the node keeps 8192-bit vectors:
 *
 *   ITEM SHARDING  (--slice -1, default)  vector i is a WHOLE concept, ids g_base..g_base+n.
 *                  The cluster holds many concepts at 8192 bits. Max capacity.
 *
 *   DIM SHARDING   (--slice K)            vector i is bits [K*8192,(K+1)*8192) of concept i.
 *                  Every node holds a piece of EVERY concept, so the cluster holds ONE
 *                  concept set at 8192*nodes bits. Max certainty.
 *
 * The trade is explicit and it is not free: 11 nodes give either ~220,000 concepts at 5 sigma
 * or ~20,000 concepts at 26.8 sigma. Certainty is bought with capacity. */
static int       g_slice  = -1;
static int       g_run    = 1;
static char      g_persist[256] = "/root/hdc_shard.bin";

static uint64_t  g_queries = 0, g_stores = 0;
static uint64_t  g_scan_us = 0;
static uint32_t  g_scan_max_us = 0;

static void on_sig(int s) { (void)s; g_run = 0; }

/* The one hook bench_index.c needs. Everything else about storage is invisible to it. */
static int psram_fetch(void *ctx, uint64_t offset, hd_t out)
{
    (void)ctx;
    if (!g_have_psram) return 0;
    return (pb_read(&g_ps, offset, out, HD_BYTES) == 0) ? 1 : 0;
}

static uint64_t now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)(t.tv_nsec / 1000);
}

/* ===========================================================================================
 * PERSISTENCE -- the microSD is why a node can be power-cycled without losing what it knows
 * -------------------------------------------------------------------------------------------
 * Written to a temp file and renamed. A half-written shard file that still has the right
 * magic is worse than no file: the node comes up believing corrupt memories, and every query
 * it answers is confidently wrong. rename() is atomic on ext4, so the file is either the old
 * one or the new one and never a mixture.
 * =========================================================================================*/
#define SHARD_MAGIC 0x48444331u   /* "HDC1" */

static int shard_save(void)
{
    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g_persist);

    FILE *f = fopen(tmp, "wb");
    if (!f) { fprintf(stderr, "save: %s: %s\n", tmp, strerror(errno)); return -1; }

    uint32_t hdr[4] = { SHARD_MAGIC, HD_BITS, g_mem.n, g_base };
    int ok = (fwrite(hdr, sizeof(hdr), 1, f) == 1);
    if (ok && g_mem.n) {
        ok = (fwrite(g_store, HD_BYTES, g_mem.n, f) == g_mem.n) &&
             (fwrite(g_label, sizeof(uint16_t), g_mem.n, f) == g_mem.n);
    }
    if (ok) ok = (fflush(f) == 0);
    fclose(f);
    if (!ok) { unlink(tmp); return -1; }

    if (rename(tmp, g_persist) != 0) { unlink(tmp); return -1; }
    return 0;
}

static int shard_load(void)
{
    FILE *f = fopen(g_persist, "rb");
    if (!f) return -1;

    uint32_t hdr[4];
    if (fread(hdr, sizeof(hdr), 1, f) != 1) { fclose(f); return -1; }

    /* Refuse a shard built at a different dimension rather than reading it as garbage. A
     * node rebuilt with HD_BITS=4096 loading an 8192-bit file would produce plausible-looking
     * distances that mean nothing at all. */
    if (hdr[0] != SHARD_MAGIC || hdr[1] != HD_BITS) {
        fprintf(stderr, "shard: wrong magic or dimension (file %u bits, build %u)\n",
                (unsigned)hdr[1], (unsigned)HD_BITS);
        fclose(f);
        return -1;
    }
    if (hdr[2] > g_mem.cap) { fclose(f); return -1; }

    const uint32_t n = hdr[2];
    g_base = (uint16_t)hdr[3];
    if (n && (fread(g_store, HD_BYTES, n, f) != n ||
              fread(g_label, sizeof(uint16_t), n, f) != n)) {
        fclose(f);
        return -1;
    }
    g_mem.n = (uint16_t)n;
    fclose(f);
    return 0;
}

/* ===========================================================================================
 * MAIN
 * =========================================================================================*/
static void usage(const char *a)
{
    fprintf(stderr,
        "usage: %s [--node N] [--shard N] [--base N] [--port N] [--file PATH] [--seed-random N]\n"
        "  --node N        node id 0..63 (must be unique; the merge dedups by it)\n"
        "  --shard N       capacity in hypervectors (%d bytes each)\n"
        "  --base N        this shard's first GLOBAL index\n"
        "  --port N        UDP port (default 9000)\n"
        "  --file PATH     persistence file (default /root/hdc_shard.bin)\n"
        "  --seed-random N fill with N derived vectors for a bench test\n", a, HD_BYTES);
}

int main(int argc, char **argv)
{
    uint32_t cap = 20000;          /* 20k x 1 KB = 20 MB, comfortable inside 64 MB with CMA  */
    int port = 9000;
    uint32_t seed_random = 0;
    uint32_t psram_hz = 25000000u;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--node")   && i + 1 < argc) g_node = (uint16_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--shard")&& i + 1 < argc) cap = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--base") && i + 1 < argc) g_base = (uint16_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--file") && i + 1 < argc)
            snprintf(g_persist, sizeof(g_persist), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--seed-random") && i + 1 < argc)
            seed_random = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--slice") && i + 1 < argc) g_slice = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dream")) g_dreaming = 1;
        else if (!strcmp(argv[i], "--psram") && i + 1 < argc) g_psram_chips = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--psram-hz") && i + 1 < argc) psram_hz = (uint32_t)atoi(argv[++i]);
        else { usage(argv[0]); return 1; }
    }

    if (g_node >= 64) {
        fprintf(stderr, "node id must be < 64: the coordinator dedups replies with a 64-bit\n"
                        "mask, and an untracked node inflates coverage, which is how a\n"
                        "silent node turns into a confidently wrong answer.\n");
        return 1;
    }
    if (cap > 65000u) { fprintf(stderr, "shard cap is a uint16 index: max 65000\n"); return 1; }

    g_store = (hd_t *)malloc((size_t)cap * sizeof(hd_t));
    g_label = (uint16_t *)malloc((size_t)cap * sizeof(uint16_t));
    if (!g_store || !g_label) {
        fprintf(stderr, "out of memory for %u vectors (%.1f MB). The Luckfox has 64 MB and the\n"
                        "stock config reserves 24 MB of it to CMA for the ISP -- if the camera\n"
                        "is unused, rebuilding with CMA=4M returns 20 MB.\n",
                (unsigned)cap, (double)cap * HD_BYTES / 1048576.0);
        return 1;
    }
    hd_mem_init(&g_mem, g_store, g_label, (uint16_t)cap);

    if (g_dreaming) {
        /* Prototypes are far fewer than instances by construction -- that is the compression.
         * cap/8 has been generous in every test so far (12 categories from 240 instances), and
         * running out only stops NEW concepts forming; nothing is lost. */
        const uint32_t pcap = (cap / 8u) < 16u ? 16u : (cap / 8u);
        g_pstore   = (hd_t *)malloc((size_t)pcap * sizeof(hd_t));
        g_plabel   = (uint16_t *)malloc((size_t)pcap * sizeof(uint16_t));
        g_support  = (uint16_t *)malloc((size_t)pcap * sizeof(uint16_t));
        g_absorbed = (uint8_t *)malloc((size_t)cap);
        g_work     = (hd_acc_t *)malloc(sizeof(hd_acc_t));   /* 16 KB, never on a stack */
        if (!g_pstore || !g_plabel || !g_support || !g_absorbed || !g_work) {
            fprintf(stderr, "out of memory for the dream layer (%.1f MB extra)\n",
                    (double)pcap * HD_BYTES / 1048576.0);
            return 1;
        }
        hd_mem_init(&g_proto, g_pstore, g_plabel, (uint16_t)pcap);
        dream_init(&g_dream, &g_mem, &g_proto, g_support, g_absorbed, g_work);
        printf("dream layer  : %u prototype slots (%.1f MB), consolidating when idle\n",
               (unsigned)pcap, (double)pcap * HD_BYTES / 1048576.0);
    }

    /* --- deep tier: open the stick, index in DDR2, bodies on PSRAM --- */
    if (g_psram_chips > 0) {
        if (pb_open(&g_ps, "/dev/spidev0.0", psram_hz, (uint8_t)g_psram_chips) != 0) {
            fprintf(stderr, "psram: open failed -- run psram_bringup first\n");
            return 1;
        }
        g_have_psram = 1;

        const uint64_t bodies = pb_capacity(&g_ps) / HD_BYTES;
        uint32_t icap = (uint32_t)bodies;
        if (icap > 60000000u) icap = 60000000u;

        g_ientry = (hd_entry_t *)malloc((size_t)icap * sizeof(hd_entry_t));
        g_ilabel = (uint16_t *)malloc((size_t)icap * sizeof(uint16_t));
        if (!g_ientry || !g_ilabel) {
            fprintf(stderr, "psram: index needs %.1f MB of DDR2 for %s bodies -- too big.\n"
                    "  use fewer chips, or raise HD_INDEX_BITS to shrink nothing (it grows).\n",
                    (double)icap * sizeof(hd_entry_t) / 1048576.0,
                    "that many");
            return 1;
        }
        hd_index_init(&g_index, g_ientry, g_ilabel, icap, psram_fetch, NULL);
        g_use_index = 1;

        printf("deep tier    : %d chips, %llu MB of bodies, index %u entries (%.1f MB DDR2)\n",
               g_psram_chips,
               (unsigned long long)(pb_capacity(&g_ps) / 1048576u),
               (unsigned)icap, (double)icap * sizeof(hd_entry_t) / 1048576.0);
    }

    if (shard_load() == 0)
        printf("loaded %u vectors from %s (base %u)\n", g_mem.n, g_persist, g_base);
    else
        printf("no shard on disk, starting empty\n");

    if (seed_random) {
        for (uint32_t i = 0; i < seed_random && g_mem.n < cap; i++) {
            hd_t v;
            uint32_t s = 0x51EED000u + g_base + i;
            hd_random(v, &s);
            hd_mem_add(&g_mem, v, (uint16_t)(g_base + i));
        }
        printf("seeded %u derived vectors\n", (unsigned)seed_random);
    }

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { perror("socket"); return 1; }
    struct sockaddr_in me;
    memset(&me, 0, sizeof(me));
    me.sin_family = AF_INET;
    me.sin_addr.s_addr = htonl(INADDR_ANY);
    me.sin_port = htons((uint16_t)port);
    if (bind(sock, (struct sockaddr *)&me, sizeof(me)) < 0) { perror("bind"); return 1; }

    signal(SIGINT,  on_sig);
    signal(SIGTERM, on_sig);

    printf("node %u  shard %u/%u  base %u  udp %d  %.1f MB resident\n",
           (unsigned)g_node, (unsigned)g_mem.n, (unsigned)cap, (unsigned)g_base, port,
           (double)cap * HD_BYTES / 1048576.0);
    if (g_slice >= 0)
        printf("layout: DIMENSION shard, slice %d -- full concept is %u bits wide\n",
               g_slice, (unsigned)HD_BITS);
    else
        printf("layout: ITEM shard, whole concepts, ids %u..%u\n",
               (unsigned)g_base, (unsigned)(g_base + g_mem.n));
    printf("ready. degrade, never fail.\n");
    if (g_dreaming)
        printf("it will form concepts while idle. leave it running.\n");
    fflush(stdout);

    uint8_t buf[QUERY_BYTES + 64];
    static hd_scan_t scan;            /* static: hd_scan_t carries a 1 KB query */

    while (g_run) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(sock, &rd);
        struct timeval tv = { 2, 0 };
        const int r = select(sock + 1, &rd, NULL, NULL, &tv);
        if (r < 0) { if (errno == EINTR) continue; perror("select"); break; }

        if (r == 0) {
            /* NOTHING IS BEING ASKED. This is when the machine thinks about what it already
             * knows. The budget is deliberately small so a query arriving mid-pass waits at
             * most one chunk -- the same interruptibility contract the ESP32 shard worker
             * uses, for the same reason. */
            if (g_dreaming && g_mem.n) {
                const uint16_t formed = dream_consolidate(&g_dream, &g_dcursor, 24);
                g_dpasses++;
                if (formed)
                    printf("dream: +%u concept(s)  now %u prototypes from %u instances\n",
                           formed, (unsigned)g_proto.n, (unsigned)g_mem.n);
                fflush(stdout);
            }
            continue;
        }

        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        const ssize_t n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
        if (n < QUERY_HDR) continue;

        msg_hdr_t h;
        hdr_unpack(&h, buf);

        if (h.type == MSG_QUERY && n >= (ssize_t)QUERY_BYTES) {
            hd_t q;
            memcpy(q, buf + QUERY_HDR, HD_BYTES);

            if (g_use_index) {
                hd_index_result_t ir;
                hd_index_search(&g_index, q, &ir);

                hd_partial_t pp;
                hd_partial_none(&pp, h.query_id);
                pp.node = g_node;
                pp.base = g_base;
                pp.total = (uint16_t)(g_index.n > 65535u ? 65535u : g_index.n);

                if (ir.status == HD_INDEX_OK) {
                    pp.best_local = (uint16_t)ir.slot;
                    pp.best_dist  = ir.dist;
                    pp.scanned    = pp.total;
                } else {
                    /* TOO_NOISY or nothing found. scanned stays 0 so the coordinator counts
                     * this node's memory as UNSEARCHED rather than searched-and-empty --
                     * the difference between "not here" and "could not tell". */
                    pp.scanned = 0;
                }
                uint8_t out[QUERY_HDR + HD_PARTIAL_WIRE_BYTES];
                msg_hdr_t rh = { MSG_REPLY, 0, h.query_id, 0, 0 };
                hdr_pack(&rh, out);
                hd_partial_pack(&pp, out + QUERY_HDR);
                sendto(sock, out, sizeof(out), 0, (struct sockaddr *)&from, fl);
                g_queries++;
                continue;
            }

            /* Prototypes first: few, general, and they answer "what KIND of thing is this"
             * for a fraction of the work. Only if no concept recognises it does the full
             * instance scan run. General fast, specific slow -- the order a person answers in. */
            uint8_t reply_flags = 0;
            if (g_dreaming && g_proto.n) {
                uint32_t pd = 0;
                const int32_t ps = hd_mem_best(&g_proto, q, &pd);
                if (ps >= 0 && pd <= (uint32_t)((HD_BITS * 2u) / 5u)) {
                    reply_flags = HDC_FLAG_PROTOTYPE;
                    uint8_t out[QUERY_HDR + HD_PARTIAL_WIRE_BYTES];
                    msg_hdr_t rh = { MSG_REPLY, reply_flags, h.query_id, 0, 0 };
                    hdr_pack(&rh, out);
                    hd_partial_t pp;
                    hd_partial_none(&pp, h.query_id);
                    pp.node = g_node;
                    pp.base = g_base;
                    pp.best_local = (uint16_t)ps;
                    pp.best_dist  = pd;
                    pp.scanned = g_mem.n;      /* a concept covers everything it absorbed */
                    pp.total   = g_mem.n;
                    hd_partial_pack(&pp, out + QUERY_HDR);
                    sendto(sock, out, sizeof(out), 0, (struct sockaddr *)&from, fl);
                    g_queries++;
                    continue;
                }
            }

            hd_scan_begin(&scan, &g_mem, q, g_node, h.query_id, g_base);

            const uint64_t t0 = now_us();
            const uint64_t deadline = t0 + (h.deadline_ms ? h.deadline_ms : 50u) * 1000u;

            /* Chunked even though Linux preempts us anyway. The deadline is the point: a node
             * that is swapping or throttled answers late with partial coverage instead of
             * making the coordinator wait for a scan that may never finish. */
            while (!hd_scan_chunk(&scan, 256)) {
                if (now_us() >= deadline) break;
            }
            const uint64_t dt = now_us() - t0;
            g_scan_us += dt;
            if (dt > g_scan_max_us) g_scan_max_us = (uint32_t)dt;
            g_queries++;

            uint8_t out[QUERY_HDR + HD_PARTIAL_WIRE_BYTES];
            msg_hdr_t rh = { MSG_REPLY, 0, h.query_id, 0, 0 };
            hdr_pack(&rh, out);
            hd_partial_pack(&scan.result, out + QUERY_HDR);
            sendto(sock, out, sizeof(out), 0, (struct sockaddr *)&from, fl);

        } else if (h.type == MSG_STORE && n >= (ssize_t)QUERY_BYTES) {
            hd_t v;
            memcpy(v, buf + QUERY_HDR, HD_BYTES);

            /* One-shot learning, over the wire. With the dream layer this is SURPRISE-GATED:
             * familiar input sharpens what is already known and costs no memory, and only
             * genuinely new input is stored. Measured: a novice stores 100% of what it sees,
             * the same node after forming concepts stores 24% and recognises the rest. */
            int32_t slot;
            if (g_use_index) {
                /* Body to the stick, prefix to the index. The body offset is a bump
                 * allocator: this log only ever grows, exactly like bench_store. */
                if (g_body_next + HD_BYTES > pb_capacity(&g_ps)) {
                    slot = -1;                       /* stick full */
                } else if (pb_write(&g_ps, g_body_next, v, HD_BYTES) != 0) {
                    slot = -1;
                } else {
                    slot = hd_index_add(&g_index, v, h.label, g_body_next);
                    if (slot >= 0) g_body_next += HD_BYTES;
                }
            } else if (g_dreaming) {
                const dream_result_t dr = dream_observe(&g_dream, v, h.label);
                slot = (dr == DREAM_FULL) ? -1 : 0;
            } else {
                slot = hd_mem_add(&g_mem, v, h.label);
            }
            if (slot >= 0) {
                g_stores++;
                if ((g_stores % 256u) == 0u) shard_save();   /* periodic, not per-store: the
                                                              * SD card is the slow part and a
                                                              * lost 256 is cheap to relearn */
            }
            /* Acknowledge with a well-formed empty partial so the coordinator can parse
             * every reply with one code path. `scanned`/`total` stay zero: a STORE ack must
             * never look like search coverage. */
            hd_partial_t ack;
            hd_partial_none(&ack, h.query_id);
            ack.node = g_node;
            ack.base = g_base;

            uint8_t out[QUERY_HDR + HD_PARTIAL_WIRE_BYTES];
            msg_hdr_t rh = { MSG_REPLY, (uint8_t)(slot >= 0 ? 1 : 0), h.query_id, 0, 0 };
            hdr_pack(&rh, out);
            hd_partial_pack(&ack, out + QUERY_HDR);
            sendto(sock, out, sizeof(out), 0, (struct sockaddr *)&from, fl);

        } else if (h.type == MSG_DEEP && n >= (ssize_t)(QUERY_HDR + 4)) {
            /* Stage 2: the coordinator has a shortlist and wants THIS node's slice scored.
             * The request carries candidate ids only -- no vector crosses the wire. */
            uint16_t qid2 = 0, k = 0;
            static uint32_t cand[HD_MAX_K];
            static uint16_t part[HD_MAX_K];

            if (!hd_deep_unpack_req(&qid2, &k, cand, HD_MAX_K,
                                    buf + QUERY_HDR, (uint32_t)(n - QUERY_HDR)))
                continue;
            if (n < (ssize_t)(QUERY_HDR + HD_DEEP_REQ_BYTES(k) + HD_BYTES)) continue;

            hd_t q;
            memcpy(q, buf + QUERY_HDR + HD_DEEP_REQ_BYTES(k), HD_BYTES);

            hd_deep_score(&g_mem, q, 0, (uint16_t)HD_WORDS, cand, k, g_base, part);

            uint8_t out[QUERY_HDR + 6 + 2 * HD_MAX_K];
            msg_hdr_t rh = { MSG_DEEPREP, 0, qid2, 0, 0 };
            hdr_pack(&rh, out);
            const uint32_t nb = hd_deep_pack_rep(qid2, g_node, part, k,
                                                 out + QUERY_HDR, sizeof(out) - QUERY_HDR);
            if (nb) sendto(sock, out, QUERY_HDR + nb, 0, (struct sockaddr *)&from, fl);
            g_queries++;

        } else if (h.type == MSG_STAT) {
            char s[256];
            const int len = snprintf(s, sizeof(s),
                "node=%u n=%u cap=%u base=%u queries=%llu stores=%llu "
                "mean_scan_us=%llu max_scan_us=%u proto=%u observed=%u "
                "reinforced=%u merges=%u dream_passes=%llu",
                (unsigned)g_node, (unsigned)g_mem.n, (unsigned)cap, (unsigned)g_base,
                (unsigned long long)g_queries, (unsigned long long)g_stores,
                (unsigned long long)(g_queries ? g_scan_us / g_queries : 0),
                (unsigned)g_scan_max_us,
                (unsigned)g_proto.n, (unsigned)g_dream.observed,
                (unsigned)g_dream.reinforced, (unsigned)g_dream.merges,
                (unsigned long long)g_dpasses);
            sendto(sock, s, (size_t)len, 0, (struct sockaddr *)&from, fl);
        }
    }

    printf("\nshutting down: %llu queries, %llu stores\n",
           (unsigned long long)g_queries, (unsigned long long)g_stores);
    if (g_dreaming)
        printf("it formed %u concepts from %u instances over %llu idle passes\n",
               (unsigned)g_proto.n, (unsigned)g_mem.n, (unsigned long long)g_dpasses);
    if (shard_save() == 0) printf("shard saved to %s (%u vectors)\n", g_persist, g_mem.n);
    if (g_have_psram) pb_close(&g_ps);
    free(g_ientry);
    free(g_ilabel);
    close(sock);
    free(g_store);
    free(g_label);
    return 0;
}
