/* ===========================================================================================
 *  stage_link.h -- passing activations between pipeline stages
 * ===========================================================================================
 *
 *  WHY THIS IS NOT THE OTHER PROTOCOL IN THIS REPO
 *  ------------------------------------------------
 *  bench_hdc_shard.c is built on a monoid so that a node can answer partially, late, or twice and
 *  the merge still lands on the same result. That is the right shape for an associative memory
 *  search, where a missing shard costs coverage and nothing else.
 *
 *  Pipeline inference has the opposite character. Layer 7's output is layer 8's input. Skipping a
 *  layer does not degrade the answer, it destroys it, and there is no algebra that repairs the
 *  damage afterwards. So this link is RELIABLE AND ORDERED, which means TCP, and a lost message is
 *  a failed token rather than a lower-confidence one.
 *
 *  Getting that distinction wrong would look like it worked. The output would be text. It would
 *  just be the wrong text, occasionally, with nothing in the system able to tell.
 *
 *
 *  WHY THE PAYLOAD IS SMALL AND THE WEIGHTS NEVER MOVE
 *  ----------------------------------------------------
 *  A stage boundary carries one activation vector: hidden dimension times two bytes, so roughly
 *  16 KB for a large model. The weights that produced it are gigabytes and stay where they are.
 *  That asymmetry is the entire reason this architecture can work over ordinary ethernet:
 *
 *      32 hops x 16 KB = 512 KB per token
 *      over 1 Gbit     = 4.1 ms      -> a 244 tok/s ceiling
 *
 *  Ten times more headroom than this machine will use. It is also why tensor parallelism is not
 *  on the table: that would all-reduce the hidden state across every node at every layer, tens of
 *  megabytes per token, and no interconnect in this budget survives it.
 *
 *
 *  MEASURING LATENCY WITHOUT SYNCHRONISED CLOCKS
 *  ----------------------------------------------
 *  Two boards do not share a clock, and one-way latency computed from two unsynchronised clocks is
 *  the clock offset plus noise, which can even come out negative. So the sender stamps `t_emit_us`
 *  from its own clock, the receiver ECHOES that value back untouched in the acknowledgement, and
 *  the sender subtracts against its own clock again. Round trip is then measured entirely on one
 *  clock and no synchronisation is needed or claimed.
 *
 *  One-way cost is reported as half the round trip, with the caveat that it assumes a symmetric
 *  path. Over USB networking that assumption is worth doubting and the figure is labelled.
 *
 *
 *  WHY A SEQUENCE NUMBER ON TOP OF TCP
 *  ------------------------------------
 *  TCP already guarantees order and delivery on a connection. The sequence number catches what TCP
 *  cannot: a stage that reconnected and started over, a message assembled from a partial read, or
 *  two senders wired to the same port by mistake. Every one of those produces a stream that is
 *  individually well formed and collectively wrong, and a monotonic counter is the cheapest way to
 *  notice.
 * ===========================================================================================
 */

#ifndef STAGE_LINK_H
#define STAGE_LINK_H

#include <stdint.h>
#include <stddef.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef SOCKET stage_fd_t;
  #define STAGE_BAD_FD INVALID_SOCKET
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  typedef int stage_fd_t;
  #define STAGE_BAD_FD (-1)
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define STAGE_MAGIC   0x42314E31u    /* "B1N1" */
#define STAGE_VERSION 2
#define STAGE_MAX_PAYLOAD (256u * 1024u)

enum {
    STAGE_HELLO = 1,   /* who I am, what layers I hold                                    */
    STAGE_ACT   = 2,   /* an activation moving forward                                    */
    STAGE_ACK   = 3,   /* echoes t_emit_us so the sender can time a round trip            */
    STAGE_BYE   = 4,
    /* The projection lap. The payload is a normed activation followed by one (token id, logit) entry
     * per stage: each node fills in its own slot from its own slice of the vocabulary, so what comes
     * back to the head is a handful of candidates rather than 594 KB of logits. */
    STAGE_LOGITS = 5
};

/* Fixed 32 bytes, little-endian on the wire, no padding assumptions.
 *
 * Written and read field by field rather than memcpy'd as a struct, because two nodes here have
 * different compilers and a struct layout that happens to match today is not a protocol. */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint32_t token_id;
    uint16_t stage;
    uint16_t layer;
    uint32_t len;
    uint32_t seq;        /* position in the SEQUENCE, which every node must agree on          */
    uint64_t t_emit_us;
    /* WHICH SEQUENCE this activation belongs to.
     *
     * Version 1 had no such field because it assumed one conversation at a time, and that assumption
     * cost n-1 of every n nodes: with a single token in flight only one stage is ever working. Adding
     * four bytes here is what lets four independent sequences move through the ring at once and turns
     * a capacity machine into a throughput machine.
     *
     * A node keeps one KV cache per stream, so this field selects which one the layer arithmetic
     * reads and writes. Get it wrong and two conversations silently blend, which reads as the model
     * losing the thread rather than as a bug. */
    uint32_t stream;
    uint32_t reserved;   /* keeps the header 8-byte aligned and leaves room for the next idea */
} stage_hdr_t;

#define STAGE_HDR_BYTES 40

void stage_hdr_pack  (const stage_hdr_t *h, uint8_t out[STAGE_HDR_BYTES]);
int  stage_hdr_unpack(stage_hdr_t *h, const uint8_t in[STAGE_HDR_BYTES]);

/* Start up / shut down sockets. On POSIX these do nothing; on Windows they run WSAStartup, which
 * must happen before any socket call and is the usual reason a first attempt fails there. */
int  stage_init(void);
void stage_shutdown(void);

/* Microseconds from a monotonic clock. Never wall time: an NTP step mid-measurement would produce
 * a negative interval, and on a board with no RTC wall time starts at the epoch anyway. */
uint64_t stage_now_us(void);

stage_fd_t stage_listen (uint16_t port);
stage_fd_t stage_accept (stage_fd_t server);
stage_fd_t stage_connect(const char *host, uint16_t port);
void       stage_close  (stage_fd_t fd);

/* Nagle's algorithm holds a small write back for up to 40 ms hoping to coalesce it with the next
 * one. For bulk transfer that is a win; for a request-response pattern of small messages it is
 * catastrophic and looks exactly like network latency. Disabled on every connection here. */
int stage_nodelay(stage_fd_t fd);

/* Send and receive a whole message. Both loop until the full length is transferred, because a
 * single send() or recv() is permitted to move fewer bytes than asked and assuming otherwise is
 * the most common way socket code breaks under load rather than in testing. */
int stage_send(stage_fd_t fd, const stage_hdr_t *h, const void *payload);
int stage_recv(stage_fd_t fd, stage_hdr_t *h, void *payload, uint32_t max_payload);

#ifdef __cplusplus
}
#endif
#endif /* STAGE_LINK_H */
