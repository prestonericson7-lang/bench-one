/*
 * libfpgagpu.c -- Orange Pi host library for the FPGA-GPU. See libfpgagpu.h.
 *
 * Implementation notes
 *  - Both links (daemon TCP, Teensy tty/TCP) use the same framing: 12-byte gpu_msg_hdr + payload,
 *    replies of type | 0x8000 starting with int32 status. Descriptors are non-blocking; every
 *    write and read waits with poll(). Write timeouts and reply-continuation timeouts are
 *    inactivity timeouts (reset whenever bytes move), so large transfers that make progress
 *    never time out, while a dead peer is detected after timeout_ms.
 *  - A reply of an unexpected type is skipped (its payload drained). On the Teensy link the reader
 *    also hunts for the magic byte by byte, so junk on the serial line cannot desynchronise it.
 *    On the daemon link a bad magic means the TCP stream is corrupt: the connection is closed.
 *  - Static builds: no getaddrinfo/glob/getpwnam (they need NSS shared objects at run time);
 *    host names come from /etc/hosts, the Teensy is found with opendir().
 */
#define _GNU_SOURCE
#include "libfpgagpu.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "libfpgagpu sends host structs: little-endian hosts only (the wire format is little-endian)"
#endif

typedef char fgpu_chk_sizes[(sizeof(gpu_msg_hdr) == 12 && sizeof(net_vtx) == 16 && sizeof(net_tri) == 56 &&
                             sizeof(net_rect) == 28 && sizeof(net_sprite_upload) == 8 &&
                             sizeof(net_sprite_draw) == 16 && sizeof(net_set_config) == 8 &&
                             sizeof(net_hello_reply) == 32 && sizeof(net_status_reply) == 112 &&
                             sizeof(net_wait_frame) == 8 && sizeof(net_wait_frame_reply) == 8 &&
                             sizeof(net_sync_reply) == 8 && sizeof(net_sprite_upload_reply) == 12 &&
                             sizeof(net_frame_get) == 12 && sizeof(net_frame_get_reply) == 16 &&
                             sizeof(t_hello_reply) == 28 && sizeof(t_frame_reply) == 36 &&
                             sizeof(t_stats_reply) == 56 && sizeof(t_mesh_hdr) == 12 &&
                             sizeof(t_scene_hdr) == 156 && sizeof(t_scene_obj) == 88 && sizeof(t_auto) == 8 &&
                             sizeof(geom_vertex) == 28 && sizeof(geom_draw) == 72 &&
                             sizeof(geom_frame_hdr) == 88) ? 1 : -1];

#define T_MAX_DRAWS         256u          /* TG_MAX_DRAWS of the firmware (teensy_gpu/tg_core.h) */
#define NET_TRIS_CHUNK      65536u        /* triangles per NET_TRIS message (3.6 MB) */
#define NET_RECS_CHUNK      16384u        /* records per NET_RECORDS message (1.5 MB) */
#define T_RECS_CHUNK        2048u         /* records per T_RECORDS message (192 KB) */
#define MAX_SKIPPED_REPLIES 8
#define MAX_HUNT_BYTES      (1u << 20)
#define T_SLOW_TIMEOUT      15000         /* ms: T_FRAME / T_RECORDS may wait for BUSY */

/* ============================================================================================ */
/* time, errors                                                                                   */
/* ============================================================================================ */
static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

double fgpu_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

void fgpu_sleep_ms(int ms)
{
    struct timespec ts;
    if (ms <= 0)
        return;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
        ;
}

const char *fgpu_strerror(int code)
{
    switch (code) {
    case GPU_ERR_OK:        return "ok";
    case GPU_ERR_PROTO:     return "GPU_ERR_PROTO (malformed message / not allowed for this client)";
    case GPU_ERR_NOPL:      return "GPU_ERR_NOPL (PL not configured / ID mismatch)";
    case GPU_ERR_ARG:       return "GPU_ERR_ARG (bad argument)";
    case GPU_ERR_NOMEM:     return "GPU_ERR_NOMEM (pool / mesh store full)";
    case GPU_ERR_TIMEOUT:   return "GPU_ERR_TIMEOUT";
    case GPU_ERR_BUS:       return "GPU_ERR_BUS (Teensy: FPGA bus not ready)";
    case FGPU_ERR_IO:       return "connection error";
    case FGPU_ERR_LTIMEOUT: return "no reply (local timeout)";
    case FGPU_ERR_REPLY:    return "malformed reply";
    case FGPU_ERR_STATE:    return "call not valid in this state";
    case FGPU_ERR_NOMEM:    return "out of memory";
    case FGPU_ERR_ARG:      return "bad argument";
    default:                return "unknown error";
    }
}

/* ============================================================================================ */
/* link: framing over a non-blocking descriptor                                                   */
/* ============================================================================================ */
typedef struct {
    int      fd;
    int      is_sock;
    int      hunt;              /* resynchronise on the magic (serial line) */
    uint32_t magic;
    int      timeout_ms;
    int      broken;
    char     err[256];
    unsigned long long tx, rx, junk;
} link_t;

static int lfail(link_t *l, int code, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static int lfail(link_t *l, int code, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(l->err, sizeof l->err, fmt, ap);
    va_end(ap);
    if (code == FGPU_ERR_IO || code == FGPU_ERR_LTIMEOUT)
        l->broken = 1;
    return code;
}

static int link_usable(link_t *l)
{
    if (l->fd < 0)
        return lfail(l, FGPU_ERR_IO, "not connected");
    if (l->broken)
        return FGPU_ERR_IO;             /* keep the message of the original failure */
    return 0;
}

/* wait for `ev` on the descriptor for at most ms; 1 = ready, 0 = timeout, <0 = error */
static int wait_fd(link_t *l, short ev, int ms)
{
    for (;;) {
        struct pollfd p;
        int r;
        p.fd = l->fd;
        p.events = ev;
        p.revents = 0;
        r = poll(&p, 1, ms < 0 ? 0 : ms);
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0)
            return lfail(l, FGPU_ERR_IO, "poll: %s", strerror(errno));
        if (r == 0)
            return 0;
        if (p.revents & POLLNVAL)
            return lfail(l, FGPU_ERR_IO, "descriptor closed");
        return 1;                       /* POLLERR/POLLHUP: the read/write reports the details */
    }
}

/* write every byte of iov[0..cnt) (the array is modified) */
static int link_writev(link_t *l, struct iovec *iov, int cnt)
{
    uint64_t last = now_ms();
    int r0 = link_usable(l);
    if (r0 < 0)
        return r0;
    while (cnt > 0) {
        ssize_t r;
        if (iov->iov_len == 0) {
            iov++;
            cnt--;
            continue;
        }
        if (l->is_sock) {
            struct msghdr m;
            memset(&m, 0, sizeof m);
            m.msg_iov = iov;
            m.msg_iovlen = (size_t)cnt;
            r = sendmsg(l->fd, &m, MSG_NOSIGNAL);
        } else {
            r = writev(l->fd, iov, cnt);
        }
        if (r > 0) {
            size_t n = (size_t)r;
            l->tx += n;
            last = now_ms();
            while (n > 0 && cnt > 0) {
                if (n >= iov->iov_len) {
                    n -= iov->iov_len;
                    iov++;
                    cnt--;
                } else {
                    iov->iov_base = (char *)iov->iov_base + n;
                    iov->iov_len -= n;
                    n = 0;
                }
            }
            continue;
        }
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
            return lfail(l, FGPU_ERR_IO, "write failed: %s", strerror(errno));
        {
            int64_t left = (int64_t)l->timeout_ms - (int64_t)(now_ms() - last);
            int w;
            if (left <= 0)
                return lfail(l, FGPU_ERR_LTIMEOUT, "write stalled for %d ms (peer not reading)", l->timeout_ms);
            w = wait_fd(l, POLLOUT, (int)left);
            if (w < 0)
                return w;
        }
    }
    return 0;
}

static int link_send(link_t *l, uint16_t type, const void *p1, size_t n1, const void *p2, size_t n2,
                     const void *p3, size_t n3)
{
    gpu_msg_hdr h;
    struct iovec iov[4];
    uint64_t len = (uint64_t)n1 + n2 + n3;
    if (len > GPU_MSG_MAX_PAYLOAD)
        return lfail(l, FGPU_ERR_ARG, "message of %llu bytes exceeds the protocol limit",
                     (unsigned long long)len);
    h.magic = l->magic;
    h.type = type;
    h.flags = 0;
    h.len = (uint32_t)len;
    iov[0].iov_base = &h;
    iov[0].iov_len = sizeof h;
    iov[1].iov_base = (void *)(uintptr_t)p1;
    iov[1].iov_len = p1 ? n1 : 0;
    iov[2].iov_base = (void *)(uintptr_t)p2;
    iov[2].iov_len = p2 ? n2 : 0;
    iov[3].iov_base = (void *)(uintptr_t)p3;
    iov[3].iov_len = p3 ? n3 : 0;
    return link_writev(l, iov, 4);
}

/* Read exactly n bytes: up to first_ms for the first byte, then timeout_ms of inactivity. */
static int link_read(link_t *l, void *buf, size_t n, int first_ms)
{
    uint8_t *p = buf;
    size_t got = 0;
    uint64_t last = now_ms();
    int wait = first_ms;
    int r0 = link_usable(l);
    if (r0 < 0)
        return r0;
    while (got < n) {
        ssize_t r = read(l->fd, p + got, n - got);
        if (r > 0) {
            got += (size_t)r;
            l->rx += (size_t)r;
            last = now_ms();
            wait = l->timeout_ms;
            continue;
        }
        if (r == 0)
            return lfail(l, FGPU_ERR_IO, "connection closed by the peer");
        if (errno == EINTR)
            continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            return lfail(l, FGPU_ERR_IO, "read failed: %s", strerror(errno));
        {
            int64_t left = (int64_t)wait - (int64_t)(now_ms() - last);
            int w;
            if (left <= 0) {
                if (got)
                    return lfail(l, FGPU_ERR_LTIMEOUT, "reply stalled for %d ms", wait);
                return lfail(l, FGPU_ERR_LTIMEOUT, "no reply within %d ms", wait);
            }
            w = wait_fd(l, POLLIN, (int)left);
            if (w < 0)
                return w;
        }
    }
    return 0;
}

static int link_drain(link_t *l, uint32_t n)
{
    uint8_t tmp[4096];
    while (n > 0) {
        uint32_t k = n < sizeof tmp ? n : (uint32_t)sizeof tmp;
        int r = link_read(l, tmp, k, l->timeout_ms);
        if (r < 0)
            return r;
        n -= k;
    }
    return 0;
}

/* Receive the header of the next reply of type want | REPLY (other replies are skipped).
 * On success *len = payload length. */
static int reply_hdr(link_t *l, uint16_t want, uint32_t *len, int first_ms)
{
    int skipped = 0;
    for (;;) {
        gpu_msg_hdr h;
        uint8_t b[12];
        int r = link_read(l, b, sizeof b, first_ms);
        if (r < 0)
            return r;
        if (l->hunt) {
            unsigned long long hunted = 0;
            for (;;) {
                uint32_t m;
                memcpy(&m, b, 4);
                if (m == l->magic)
                    break;
                memmove(b, b + 1, sizeof b - 1);
                r = link_read(l, b + sizeof b - 1, 1, l->timeout_ms);
                if (r < 0)
                    return r;
                l->junk++;
                if (++hunted > MAX_HUNT_BYTES)
                    return lfail(l, FGPU_ERR_IO, "no valid reply header in %u bytes", MAX_HUNT_BYTES);
            }
        }
        memcpy(&h, b, sizeof h);
        if (h.magic != l->magic)
            return lfail(l, FGPU_ERR_IO, "bad reply magic 0x%08x (stream corrupt)", h.magic);
        if (h.len > GPU_MSG_MAX_PAYLOAD + 4096u)
            return lfail(l, FGPU_ERR_IO, "reply length %u out of range (stream corrupt)", h.len);
        if (h.type == (uint16_t)(want | GPU_MSG_REPLY)) {
            *len = h.len;
            return 0;
        }
        r = link_drain(l, h.len);
        if (r < 0)
            return r;
        if (++skipped > MAX_SKIPPED_REPLIES)
            return lfail(l, FGPU_ERR_REPLY, "expected reply type 0x%04x, got only others", want | GPU_MSG_REPLY);
        first_ms = l->timeout_ms;
    }
}

/* Read min(*left, n) payload bytes into dst (zero-filling the rest of n); *left is updated. */
static int reply_part(link_t *l, uint32_t *left, void *dst, uint32_t n)
{
    uint32_t k = *left < n ? *left : n;
    int r = 0;
    if (k)
        r = link_read(l, dst, k, l->timeout_ms);
    if (r < 0)
        return r;
    if (k < n)
        memset((uint8_t *)dst + k, 0, n - k);
    *left -= k;
    return 0;
}

/* Complete reply with a fixed-size layout (status first). Extra bytes -> FGPU_ERR_REPLY. */
static int reply_fixed(link_t *l, uint16_t want, void *out, uint32_t n, int first_ms)
{
    uint32_t len, left;
    int32_t st;
    int r = reply_hdr(l, want, &len, first_ms);
    if (r < 0)
        return r;
    left = len;
    r = reply_part(l, &left, out, n);
    if (r < 0)
        return r;
    if (left) {
        r = link_drain(l, left);
        if (r < 0)
            return r;
        return lfail(l, FGPU_ERR_REPLY, "reply 0x%04x has %u bytes, expected %u", want | GPU_MSG_REPLY, len, n);
    }
    if (len < 4)
        return lfail(l, FGPU_ERR_REPLY, "reply 0x%04x without a status", want | GPU_MSG_REPLY);
    memcpy(&st, out, 4);
    if (st > 0)
        return lfail(l, FGPU_ERR_REPLY, "reply status %d", st);
    return st;
}

static int link_status_call(link_t *l, uint16_t type, const void *p, size_t n, int first_ms)
{
    int32_t st;
    int r = link_send(l, type, p, n, NULL, 0, NULL, 0);
    if (r < 0)
        return r;
    return reply_fixed(l, type, &st, sizeof st, first_ms);
}

static void link_close(link_t *l)
{
    if (l->fd >= 0)
        close(l->fd);
    l->fd = -1;
}

/* ============================================================================================ */
/* TCP                                                                                            */
/* ============================================================================================ */
static int lookup_etc_hosts(const char *name, struct in_addr *a)
{
    char line[512];
    FILE *f = fopen("/etc/hosts", "r");
    int found = 0;
    if (!f)
        return -1;
    while (!found && fgets(line, sizeof line, f)) {
        char *save = NULL, *tok, *addr;
        char *hash = strchr(line, '#');
        struct in_addr cand;
        if (hash)
            *hash = 0;
        addr = strtok_r(line, " \t\r\n", &save);
        if (!addr || inet_pton(AF_INET, addr, &cand) != 1)
            continue;
        while ((tok = strtok_r(NULL, " \t\r\n", &save)) != NULL)
            if (strcasecmp(tok, name) == 0) {
                *a = cand;
                found = 1;
                break;
            }
    }
    fclose(f);
    return found ? 0 : -1;
}

static int resolve_ipv4(const char *host, struct in_addr *a)
{
    if (inet_pton(AF_INET, host, a) == 1)
        return 0;
    if (strcasecmp(host, "localhost") == 0)
        return inet_pton(AF_INET, "127.0.0.1", a) == 1 ? 0 : -1;
    return lookup_etc_hosts(host, a);
}

#define MAX_CANDIDATES 4

/* Connect to one of hosts[0..n-1] (n <= MAX_CANDIDATES). All attempts run at the same time and the
 * list order is the preference: the first host that connects is taken once every host before it
 * has failed, or has not answered within grace_ms of that success (so an unreachable first choice
 * costs grace_ms, not the whole timeout). Returns the connected fd (*chosen = its index) or an
 * FGPU_ERR_* code with the reason for every host in err. */
static int tcp_connect_any(const char *const *hosts, int n, int port, int timeout_ms, int grace_ms, int *chosen,
                           char *err, size_t errlen)
{
    int fd[MAX_CANDIDATES], st[MAX_CANDIDATES], code[MAX_CANDIDATES];   /* st: 0 pending, 1 up, -1 failed */
    char msg[MAX_CANDIDATES][200];
    uint64_t start = now_ms(), first_up = 0;
    int i, best = -1, one = 1;
    size_t pos;

    if (n > MAX_CANDIDATES)
        n = MAX_CANDIDATES;
    for (i = 0; i < n; i++) {
        struct sockaddr_in sa;
        int r;
        fd[i] = -1;
        st[i] = -1;
        code[i] = FGPU_ERR_IO;
        msg[i][0] = 0;
        memset(&sa, 0, sizeof sa);
        sa.sin_family = AF_INET;
        sa.sin_port = htons((uint16_t)port);
        if (resolve_ipv4(hosts[i], &sa.sin_addr) < 0) {
            snprintf(msg[i], sizeof msg[i], "cannot resolve '%s' (IPv4 address, localhost or /etc/hosts name)",
                     hosts[i]);
            code[i] = FGPU_ERR_ARG;
            continue;
        }
        fd[i] = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd[i] < 0) {
            snprintf(msg[i], sizeof msg[i], "socket: %s", strerror(errno));
            continue;
        }
        r = connect(fd[i], (struct sockaddr *)&sa, sizeof sa);
        if (r == 0) {
            st[i] = 1;
            if (!first_up)
                first_up = now_ms();
        } else if (errno == EINPROGRESS) {
            st[i] = 0;
        } else {
            snprintf(msg[i], sizeof msg[i], "connect %s:%d: %s", hosts[i], port, strerror(errno));
            close(fd[i]);
            fd[i] = -1;
        }
    }
    for (;;) {
        struct pollfd p[MAX_CANDIDATES];
        int idx[MAX_CANDIDATES], np = 0, k, r;
        int64_t wait;
        uint64_t now;
        for (i = 0; i < n && st[i] < 0; i++)
            ;
        if (i == n)
            break;                                      /* every host failed */
        if (st[i] == 1) {
            best = i;                                   /* the most preferred host still in the race */
            break;
        }
        now = now_ms();
        if (first_up && now >= first_up + (uint64_t)grace_ms) {
            for (i = 0; i < n && st[i] != 1; i++)
                ;
            best = i;                                   /* earlier hosts did not answer in time */
            break;
        }
        if (now >= start + (uint64_t)timeout_ms) {
            for (i = 0; i < n; i++)
                if (st[i] == 0) {
                    snprintf(msg[i], sizeof msg[i], "connect %s:%d: no answer within %d ms (cable? power? address?)",
                             hosts[i], port, timeout_ms);
                    code[i] = FGPU_ERR_LTIMEOUT;
                    st[i] = -1;
                }
            continue;
        }
        wait = (int64_t)(start + (uint64_t)timeout_ms - now);
        if (first_up && (int64_t)(first_up + (uint64_t)grace_ms - now) < wait)
            wait = (int64_t)(first_up + (uint64_t)grace_ms - now);
        for (i = 0; i < n; i++)
            if (st[i] == 0) {
                p[np].fd = fd[i];
                p[np].events = POLLOUT;
                p[np].revents = 0;
                idx[np++] = i;
            }
        r = poll(p, (nfds_t)np, (int)wait);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            for (k = 0; k < np; k++) {
                snprintf(msg[idx[k]], sizeof msg[idx[k]], "connect %s:%d: poll: %s", hosts[idx[k]], port,
                         strerror(errno));
                st[idx[k]] = -1;
            }
            continue;
        }
        for (k = 0; k < np; k++) {
            int soerr = 0;
            socklen_t sl = sizeof soerr;
            if (!p[k].revents)
                continue;
            i = idx[k];
            if (getsockopt(fd[i], SOL_SOCKET, SO_ERROR, &soerr, &sl) == 0 && soerr == 0) {
                st[i] = 1;
                if (!first_up)
                    first_up = now_ms();
            } else {
                snprintf(msg[i], sizeof msg[i], "connect %s:%d: %s", hosts[i], port, strerror(soerr ? soerr : EIO));
                st[i] = -1;
            }
        }
    }
    for (i = 0; i < n; i++)
        if (i != best && fd[i] >= 0)
            close(fd[i]);
    if (best < 0) {
        for (i = 0, pos = 0; i < n && pos < errlen; i++)
            pos += (size_t)snprintf(err + pos, errlen - pos, "%s%s", i ? "; " : "", msg[i]);
        return code[0];
    }
    *chosen = best;
    setsockopt(fd[best], IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    setsockopt(fd[best], SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
#ifdef TCP_KEEPIDLE
    {
        int idle = 10, intvl = 5, cnt = 3;
        setsockopt(fd[best], IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
        setsockopt(fd[best], IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof intvl);
        setsockopt(fd[best], IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt);
    }
#endif
    return fd[best];
}

static int tcp_connect(const char *host, int port, int timeout_ms, char *err, size_t errlen)
{
    int chosen = 0;
    return tcp_connect_any(&host, 1, port, timeout_ms, 0, &chosen, err, errlen);
}

/* ============================================================================================ */
/* daemon                                                                                         */
/* ============================================================================================ */
struct gpu_conn {
    link_t          l;
    int             observer;
    net_hello_reply hello;
    int             fg_pending;
    uint32_t        fg_scale, fg_timeout;
};

const net_hello_reply *gpu_hello_info(const gpu_conn *g) { return &g->hello; }
const char *gpu_errmsg(const gpu_conn *g) { return g ? g->l.err : "no connection"; }
int gpu_fd(const gpu_conn *g) { return g->l.fd; }
void gpu_set_timeout(gpu_conn *g, int ms) { g->l.timeout_ms = ms > 0 ? ms : FGPU_DEFAULT_TIMEOUT; }
int gpu_is_observer(const gpu_conn *g) { return g->observer; }
int gpu_frame_get_pending(const gpu_conn *g) { return g->fg_pending; }

void gpu_io_stats(const gpu_conn *g, unsigned long long *tx, unsigned long long *rx)
{
    if (tx)
        *tx = g->l.tx;
    if (rx)
        *rx = g->l.rx;
}

/* every call that sends something must not overlap a parked FRAME_GET */
static int g_ready(gpu_conn *g)
{
    if (g->fg_pending)
        return lfail(&g->l, FGPU_ERR_STATE, "a FRAME_GET is outstanding: receive its reply first");
    return link_usable(&g->l);
}

int gpu_hello(gpu_conn *g, int observer, net_hello_reply *out)
{
    uint32_t flags = GPU_HELLO_OBSERVER;
    net_hello_reply r;
    int st = g_ready(g);
    if (st < 0)
        return st;
    st = link_send(&g->l, NET_HELLO, observer ? &flags : NULL, observer ? 4u : 0u, NULL, 0, NULL, 0);
    if (st < 0)
        return st;
    st = reply_fixed(&g->l, NET_HELLO, &r, sizeof r, g->l.timeout_ms);
    if (st > -100) {                    /* the daemon answered (possibly with an error status) */
        g->hello = r;
        g->observer = observer ? 1 : 0;
    }
    if (out)
        *out = r;
    return st;
}

/* "HOST" or "HOST:PORT" (IPv4 / name, as --fpga) -> host (NUL-terminated), *port (0 if absent) */
static int parse_host_port(const char *v, char *host, size_t hostlen, int *port)
{
    const char *colon = strrchr(v, ':');
    size_t hl = colon ? (size_t)(colon - v) : strlen(v);
    *port = 0;
    if (colon) {
        char *end;
        long p;
        errno = 0;
        p = strtol(colon + 1, &end, 10);
        if (strchr(v, ':') != colon || !colon[1] || *end || errno || p < 1 || p > 65535)
            return -1;
        *port = (int)p;
    }
    if (hl == 0 || hl >= hostlen)
        return -1;
    memcpy(host, v, hl);
    host[hl] = 0;
    return 0;
}

gpu_conn *gpu_connect(const char *host, int port, int observer, char *err, size_t errlen)
{
    gpu_conn *g;
    int fd, st, n = 1, chosen = 0;
    char dummy[8], envhost[256];
    const char *cands[2];
    if (!err) {
        err = dummy;
        errlen = sizeof dummy;
    }
    err[0] = 0;
    cands[0] = host;
    if (!host || !*host) {
        const char *env = getenv(FGPU_HOST_ENV);
        if (env && *env) {
            int eport;
            if (parse_host_port(env, envhost, sizeof envhost, &eport) < 0) {
                snprintf(err, errlen, "bad %s='%s' (HOST or HOST:PORT)", FGPU_HOST_ENV, env);
                return NULL;
            }
            cands[0] = envhost;
            if (port <= 0 && eport > 0)
                port = eport;
        } else {
            cands[0] = FGPU_DEFAULT_HOST;       /* direct cable (pi_setup.sh) */
            cands[1] = FGPU_DEFAULT_HOST2;      /* car LAN (deploy/orangepi car-lan profile) */
            n = 2;
        }
    }
    if (port <= 0)
        port = GPU_NET_PORT;
    fd = tcp_connect_any(cands, n, port, FGPU_CONNECT_TIMEOUT, FGPU_AUTO_GRACE_MS, &chosen, err, errlen);
    if (fd < 0)
        return NULL;
    host = cands[chosen];
    g = calloc(1, sizeof *g);
    if (!g) {
        close(fd);
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    g->l.fd = fd;
    g->l.is_sock = 1;
    g->l.magic = GPU_NET_MAGIC;
    g->l.timeout_ms = FGPU_DEFAULT_TIMEOUT;
    st = gpu_hello(g, observer, NULL);
    if (st <= -100) {
        snprintf(err, errlen, "HELLO to %s:%d failed: %s", host, port, g->l.err);
        gpu_close(g);
        return NULL;
    }
    if (st == GPU_ERR_PROTO || g->hello.proto_version != GPU_NET_PROTO_VER) {
        snprintf(err, errlen, "%s:%d: HELLO status %d, protocol version %u (want %u)", host, port, st,
                 g->hello.proto_version, GPU_NET_PROTO_VER);
        gpu_close(g);
        return NULL;
    }
    return g;
}

void gpu_close(gpu_conn *g)
{
    if (!g)
        return;
    link_close(&g->l);
    free(g);
}

int gpu_set_config(gpu_conn *g, uint32_t control, uint16_t clear_color)
{
    net_set_config c;
    int st = g_ready(g);
    if (st < 0)
        return st;
    c.control = control;
    c.clear_color = clear_color;
    return link_status_call(&g->l, NET_SET_CONFIG, &c, sizeof c, g->l.timeout_ms);
}

int gpu_tris(gpu_conn *g, const net_tri *t, uint32_t n)
{
    int st = g_ready(g);
    if (st < 0)
        return st;
    while (n > 0) {
        uint32_t k = n < NET_TRIS_CHUNK ? n : NET_TRIS_CHUNK;
        st = link_send(&g->l, NET_TRIS, &k, 4, t, (size_t)k * sizeof *t, NULL, 0);
        if (st < 0)
            return st;
        t += k;
        n -= k;
    }
    return 0;
}

int gpu_rect(gpu_conn *g, int x0, int y0, int x1, int y1, uint16_t rgb565, float z, uint32_t flags)
{
    net_rect r;
    int st = g_ready(g);
    if (st < 0)
        return st;
    r.x0 = x0;
    r.y0 = y0;
    r.x1 = x1;
    r.y1 = y1;
    r.rgb565 = rgb565;
    r.z = z;
    r.flags = flags;
    return link_send(&g->l, NET_RECT, &r, sizeof r, NULL, 0, NULL, 0);
}

int gpu_sprite_draw(gpu_conn *g, uint16_t id, int x, int y, int colorkey_en, uint16_t colorkey)
{
    net_sprite_draw d;
    int st = g_ready(g);
    if (st < 0)
        return st;
    d.id = id;
    d.flags = colorkey_en ? 1u : 0u;
    d.x = x;
    d.y = y;
    d.colorkey = colorkey;
    return link_send(&g->l, NET_SPRITE_DRAW, &d, sizeof d, NULL, 0, NULL, 0);
}

int gpu_records(gpu_conn *g, const uint32_t (*recs)[GPU_REC_WORDS], uint32_t n)
{
    int st = g_ready(g);
    if (st < 0)
        return st;
    while (n > 0) {
        uint32_t k = n < NET_RECS_CHUNK ? n : NET_RECS_CHUNK;
        st = link_send(&g->l, NET_RECORDS, &k, 4, recs, (size_t)k * GPU_REC_BYTES, NULL, 0);
        if (st < 0)
            return st;
        recs += k;
        n -= k;
    }
    return 0;
}

int gpu_end_frame(gpu_conn *g, uint32_t frame_no)
{
    int st = g_ready(g);
    if (st < 0)
        return st;
    return link_send(&g->l, NET_END_FRAME, &frame_no, 4, NULL, 0, NULL, 0);
}

int gpu_sprite_upload(gpu_conn *g, uint16_t id, uint16_t w, uint16_t h, const uint16_t *px,
                      net_sprite_upload_reply *out)
{
    net_sprite_upload u;
    net_sprite_upload_reply r;
    int st = g_ready(g);
    if (st < 0)
        return st;
    if (w < 4 || (w % 4) != 0 || h < 1 || !px)
        return lfail(&g->l, FGPU_ERR_ARG, "sprite %u: %ux%u (w must be a multiple of 4, >= 4)", id, w, h);
    u.id = id;
    u.w = w;
    u.h = h;
    u.reserved = 0;
    st = link_send(&g->l, NET_SPRITE_UPLOAD, &u, sizeof u, px, (size_t)w * h * 2u, NULL, 0);
    if (st < 0)
        return st;
    st = reply_fixed(&g->l, NET_SPRITE_UPLOAD, &r, sizeof r, g->l.timeout_ms);
    if (out)
        *out = r;
    return st;
}

int gpu_wait_frame(gpu_conn *g, uint32_t target, uint32_t timeout_ms, uint32_t *fc)
{
    net_wait_frame w;
    net_wait_frame_reply r;
    int st = g_ready(g);
    if (st < 0)
        return st;
    w.target_frame_count = target;
    w.timeout_ms = timeout_ms;
    st = link_send(&g->l, NET_WAIT_FRAME, &w, sizeof w, NULL, 0, NULL, 0);
    if (st < 0)
        return st;
    st = reply_fixed(&g->l, NET_WAIT_FRAME, &r, sizeof r, (int)(timeout_ms > 600000u ? 600000u : timeout_ms) +
                     g->l.timeout_ms);
    if (fc)
        *fc = r.frame_count;
    return st;
}

int gpu_status(gpu_conn *g, net_status_reply *out)
{
    net_status_reply r;
    int st = g_ready(g);
    if (st < 0)
        return st;
    st = link_send(&g->l, NET_STATUS, NULL, 0, NULL, 0, NULL, 0);
    if (st < 0)
        return st;
    st = reply_fixed(&g->l, NET_STATUS, &r, sizeof r, g->l.timeout_ms);
    if (out)
        *out = r;
    return st;
}

int gpu_readback(gpu_conn *g, uint16_t *fb)
{
    uint32_t hdr[3], len, left;
    int32_t st32;
    int st = g_ready(g);
    if (st < 0)
        return st;
    st = link_send(&g->l, NET_READBACK, NULL, 0, NULL, 0, NULL, 0);
    if (st < 0)
        return st;
    st = reply_hdr(&g->l, NET_READBACK, &len, g->l.timeout_ms);
    if (st < 0)
        return st;
    left = len;
    st = reply_part(&g->l, &left, hdr, sizeof hdr);
    if (st < 0)
        return st;
    memcpy(&st32, &hdr[0], 4);
    if (len < 4)
        return lfail(&g->l, FGPU_ERR_REPLY, "READBACK reply without a status");
    if (st32 == 0) {
        if (hdr[1] != GPU_W || hdr[2] != GPU_H || left != GPU_FB_BYTES) {
            link_drain(&g->l, left);
            return lfail(&g->l, FGPU_ERR_REPLY, "READBACK reply %ux%u with %u pixel bytes", hdr[1], hdr[2], left);
        }
        return reply_part(&g->l, &left, fb, GPU_FB_BYTES);
    }
    if (left)
        st = link_drain(&g->l, left);
    return st < 0 ? st : st32;
}

int gpu_reset(gpu_conn *g)
{
    int st = g_ready(g);
    if (st < 0)
        return st;
    return link_status_call(&g->l, NET_RESET, NULL, 0, g->l.timeout_ms);
}

int gpu_sync(gpu_conn *g, uint32_t *errors)
{
    net_sync_reply r;
    int st = g_ready(g);
    if (st < 0)
        return st;
    st = link_send(&g->l, NET_SYNC, NULL, 0, NULL, 0, NULL, 0);
    if (st < 0)
        return st;
    st = reply_fixed(&g->l, NET_SYNC, &r, sizeof r, g->l.timeout_ms);
    if (errors)
        *errors = r.errors_since_last_sync;
    return st;
}

int gpu_frame_get_send(gpu_conn *g, uint32_t min_frame_no, uint32_t scale, uint32_t timeout_ms)
{
    net_frame_get f;
    int st = g_ready(g);
    if (st < 0)
        return st;
    if (scale != 1 && scale != 2)
        return lfail(&g->l, FGPU_ERR_ARG, "FRAME_GET scale %u (1 or 2)", scale);
    f.min_frame_no = min_frame_no;
    f.scale = scale;
    f.timeout_ms = timeout_ms;
    st = link_send(&g->l, NET_FRAME_GET, &f, sizeof f, NULL, 0, NULL, 0);
    if (st < 0)
        return st;
    g->fg_pending = 1;
    g->fg_scale = scale;
    g->fg_timeout = timeout_ms > 600000u ? 600000u : timeout_ms;
    return 0;
}

int gpu_frame_get_recv(gpu_conn *g, int wait_ms, uint16_t *px, net_frame_get_reply *out)
{
    net_frame_get_reply r;
    uint32_t len, left, want_w, want_h;
    int st;
    if (!g->fg_pending)
        return lfail(&g->l, FGPU_ERR_STATE, "no FRAME_GET outstanding");
    st = link_usable(&g->l);
    if (st < 0)
        return st;
    if (wait_ms >= 0) {
        st = wait_fd(&g->l, POLLIN, wait_ms);
        if (st <= 0)
            return st;
    }
    st = reply_hdr(&g->l, NET_FRAME_GET, &len, (int)g->fg_timeout + g->l.timeout_ms);
    if (st < 0)
        return st;
    g->fg_pending = 0;
    left = len;
    st = reply_part(&g->l, &left, &r, sizeof r);
    if (st < 0)
        return st;
    if (out)
        *out = r;
    if (len < 4)
        return lfail(&g->l, FGPU_ERR_REPLY, "FRAME_GET reply without a status");
    if (r.status == 0) {
        want_w = GPU_W / g->fg_scale;
        want_h = GPU_H / g->fg_scale;
        if (r.w != want_w || r.h != want_h || left != want_w * want_h * 2u) {
            link_drain(&g->l, left);
            return lfail(&g->l, FGPU_ERR_REPLY, "FRAME_GET reply %ux%u with %u bytes (want %ux%u)", r.w, r.h,
                         left, want_w, want_h);
        }
        st = reply_part(&g->l, &left, px, want_w * want_h * 2u);
        return st < 0 ? st : 1;
    }
    if (left) {
        st = link_drain(&g->l, left);
        if (st < 0)
            return st;
    }
    return 1;
}

int gpu_frame_get(gpu_conn *g, uint32_t min_frame_no, uint32_t scale, uint32_t timeout_ms, uint16_t *px,
                  net_frame_get_reply *info)
{
    net_frame_get_reply r;
    int st = gpu_frame_get_send(g, min_frame_no, scale, timeout_ms);
    if (st < 0)
        return st;
    st = gpu_frame_get_recv(g, -1, px, &r);
    if (info)
        *info = r;
    if (st < 0)
        return st;
    return r.status;
}

int gpu_wait_frame_no(gpu_conn *g, uint32_t frame_no, uint32_t timeout_ms)
{
    uint64_t deadline = now_ms() + timeout_ms;
    for (;;) {
        net_status_reply s;
        int64_t left;
        int st = gpu_status(g, &s);
        if (st < 0)
            return st;
        if ((int32_t)(gpu_reg(&s, GPU_R_LAST_FRAME_NO) - frame_no) >= 0)
            return 0;
        left = (int64_t)(deadline - now_ms());
        if (left <= 0)
            return GPU_ERR_TIMEOUT;
        st = gpu_wait_frame(g, gpu_reg(&s, GPU_R_FRAME_COUNT) + 1u, (uint32_t)(left < 1000 ? left : 1000), NULL);
        if (st < 0 && st != GPU_ERR_TIMEOUT)
            return st;
    }
}

/* ============================================================================================ */
/* Teensy                                                                                         */
/* ============================================================================================ */
struct teensy_conn {
    link_t        l;
    char          dev[300];
    int           is_tty;
    int           frame_pending;
    t_hello_reply hello;
};

const char *teensy_errmsg(const teensy_conn *t) { return t ? t->l.err : "no Teensy"; }
const char *teensy_device(const teensy_conn *t) { return t->dev; }
const t_hello_reply *teensy_hello_info(const teensy_conn *t) { return &t->hello; }
int teensy_fd(const teensy_conn *t) { return t->l.fd; }
void teensy_set_timeout(teensy_conn *t, int ms) { t->l.timeout_ms = ms > 0 ? ms : FGPU_DEFAULT_TIMEOUT; }
int teensy_frame_pending(const teensy_conn *t) { return t->frame_pending; }

void teensy_io_stats(const teensy_conn *t, unsigned long long *tx, unsigned long long *rx)
{
    if (tx)
        *tx = t->l.tx;
    if (rx)
        *rx = t->l.rx;
}

static int t_ready(teensy_conn *t)
{
    if (t->frame_pending)
        return lfail(&t->l, FGPU_ERR_STATE, "a T_FRAME is outstanding: receive its reply first");
    return link_usable(&t->l);
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* /dev/serial/by-id/ *Teensy* (sorted), else /dev/ttyACM<n> with the smallest n */
static int find_teensy(char *out, size_t n)
{
    DIR *d = opendir("/dev/serial/by-id");
    if (d) {
        char *names[64];
        int cnt = 0, i;
        struct dirent *e;
        while ((e = readdir(d)) != NULL && cnt < 64)
            if (strstr(e->d_name, "Teensy"))
                names[cnt++] = strdup(e->d_name);
        closedir(d);
        if (cnt > 0) {
            qsort(names, (size_t)cnt, sizeof names[0], cmp_str);
            snprintf(out, n, "/dev/serial/by-id/%s", names[0] ? names[0] : "");
            for (i = 0; i < cnt; i++)
                free(names[i]);
            return 0;
        }
    }
    d = opendir("/dev");
    if (d) {
        struct dirent *e;
        long best = -1;
        while ((e = readdir(d)) != NULL) {
            char *end;
            long k;
            if (strncmp(e->d_name, "ttyACM", 6) != 0 || !isdigit((unsigned char)e->d_name[6]))
                continue;
            k = strtol(e->d_name + 6, &end, 10);
            if (*end == 0 && (best < 0 || k < best))
                best = k;
        }
        closedir(d);
        if (best >= 0) {
            snprintf(out, n, "/dev/ttyACM%ld", best);
            return 0;
        }
    }
    return -1;
}

static int open_tty(const char *path, char *err, size_t errlen)
{
    struct termios tio;
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    int bits = TIOCM_DTR | TIOCM_RTS;
    if (fd < 0) {
        snprintf(err, errlen, "open %s: %s%s", path, strerror(errno),
                 errno == EACCES ? " (add yourself to the dialout group: sudo pi/pi_setup.sh, then log in again)"
                 : errno == EBUSY ? " (another program has it open)" : "");
        return FGPU_ERR_IO;
    }
    if (ioctl(fd, TIOCEXCL) < 0) {
        /* not fatal (e.g. a pty) */
    }
    if (tcgetattr(fd, &tio) < 0) {
        snprintf(err, errlen, "%s is not a serial port: %s", path, strerror(errno));
        close(fd);
        return FGPU_ERR_IO;
    }
    cfmakeraw(&tio);
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cflag &= ~(tcflag_t)(CSTOPB | PARENB | CRTSCTS);
    tio.c_cflag = (tio.c_cflag & ~(tcflag_t)CSIZE) | CS8;
    tio.c_iflag &= ~(tcflag_t)(IXON | IXOFF | IXANY);
    tio.c_cc[VMIN] = 1;                 /* with O_NONBLOCK: no data -> EAGAIN, hang-up -> 0 */
    tio.c_cc[VTIME] = 0;
    cfsetispeed(&tio, B115200);         /* USB CDC ignores the baud rate */
    cfsetospeed(&tio, B115200);
    if (tcsetattr(fd, TCSANOW, &tio) < 0) {
        snprintf(err, errlen, "tcsetattr %s: %s", path, strerror(errno));
        close(fd);
        return FGPU_ERR_IO;
    }
    ioctl(fd, TIOCMBIS, &bits);         /* DTR up: the firmware drops any half-received message */
    tcflush(fd, TCIOFLUSH);
    return fd;
}

int teensy_hello(teensy_conn *t, t_hello_reply *out)
{
    t_hello_reply r;
    int st = t_ready(t);
    if (st < 0)
        return st;
    st = link_send(&t->l, T_HELLO, NULL, 0, NULL, 0, NULL, 0);
    if (st < 0)
        return st;
    st = reply_fixed(&t->l, T_HELLO, &r, sizeof r, t->l.timeout_ms);
    if (st > -100)
        t->hello = r;
    if (out)
        *out = r;
    return st;
}

int teensy_open(const char *spec, teensy_conn **out, char *err, size_t errlen)
{
    teensy_conn *t;
    char path[300], dummy[8];
    int fd, st, is_tty = 1;

    if (!err) {
        err = dummy;
        errlen = sizeof dummy;
    }
    err[0] = 0;
    *out = NULL;
    if (spec && strcmp(spec, "none") == 0)
        return 0;
    if (!spec || !*spec || strcmp(spec, "auto") == 0) {
        if (find_teensy(path, sizeof path) < 0) {
            snprintf(err, errlen, "no Teensy found (no /dev/serial/by-id/*Teensy*, no /dev/ttyACM*)");
            return FGPU_ERR_IO;
        }
    } else {
        snprintf(path, sizeof path, "%s", spec);
    }
    if (strncmp(path, "tcp:", 4) == 0) {
        char host[256];
        const char *hp = path + 4, *colon = strrchr(hp, ':');
        long port;
        char *end;
        if (!colon || colon == hp || (size_t)(colon - hp) >= sizeof host) {
            snprintf(err, errlen, "bad Teensy spec '%s' (tcp:host:port)", path);
            return FGPU_ERR_ARG;
        }
        memcpy(host, hp, (size_t)(colon - hp));
        host[colon - hp] = 0;
        port = strtol(colon + 1, &end, 10);
        if (*end || port < 1 || port > 65535) {
            snprintf(err, errlen, "bad port in '%s'", path);
            return FGPU_ERR_ARG;
        }
        fd = tcp_connect(host, (int)port, FGPU_CONNECT_TIMEOUT, err, errlen);
        is_tty = 0;
    } else {
        fd = open_tty(path, err, errlen);
    }
    if (fd < 0)
        return fd;
    t = calloc(1, sizeof *t);
    if (!t) {
        close(fd);
        snprintf(err, errlen, "out of memory");
        return FGPU_ERR_NOMEM;
    }
    t->l.fd = fd;
    t->l.is_sock = !is_tty;
    t->l.hunt = 1;
    t->l.magic = GPU_TUSB_MAGIC;
    t->is_tty = is_tty;
    snprintf(t->dev, sizeof t->dev, "%s", path);

    /* handshake; on a tty retry once (a message that crossed the DTR edge is dropped silently) */
    t->l.timeout_ms = 1500;
    st = teensy_hello(t, NULL);
    if (st == FGPU_ERR_LTIMEOUT && is_tty) {
        t->l.broken = 0;
        tcflush(fd, TCIOFLUSH);
        t->l.timeout_ms = FGPU_DEFAULT_TIMEOUT;
        st = teensy_hello(t, NULL);
        if (st >= 0) {
            fgpu_sleep_ms(100);         /* a late answer to the first HELLO: drop it */
            tcflush(fd, TCIFLUSH);
        }
    }
    t->l.timeout_ms = FGPU_DEFAULT_TIMEOUT;
    if (st < 0) {
        snprintf(err, errlen, "%s: no geometry engine answers T_HELLO: %s", path,
                 st <= -100 ? t->l.err : fgpu_strerror(st));
        teensy_close(t);
        return st;
    }
    if (t->hello.fw_version != GPU_TUSB_FW_VER)
        snprintf(err, errlen, "warning: Teensy firmware version %u (library expects %u)", t->hello.fw_version,
                 GPU_TUSB_FW_VER);
    *out = t;
    return 0;
}

void teensy_close(teensy_conn *t)
{
    if (!t)
        return;
    link_close(&t->l);
    free(t);
}

int teensy_wait_ready(teensy_conn *t, int timeout_ms, t_hello_reply *out)
{
    uint64_t deadline = now_ms() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0);
    for (;;) {
        t_hello_reply r;
        int st = teensy_hello(t, &r);
        if (out)
            *out = r;
        if (st < 0)
            return st;
        if (r.fpga_ready && r.bus_enabled)
            return 0;
        if (now_ms() >= deadline)
            return GPU_ERR_TIMEOUT;
        fgpu_sleep_ms(20);
    }
}

int teensy_mesh(teensy_conn *t, uint16_t id, const geom_vertex *v, uint32_t nverts, const uint16_t *idx,
                uint32_t nidx)
{
    t_mesh_hdr h;
    int st = t_ready(t);
    if (st < 0)
        return st;
    if (nverts > GEOM_MAX_VERTS || nidx > GEOM_MAX_INDICES || nidx % 3 != 0 || (nverts && !v) || (nidx && !idx))
        return lfail(&t->l, FGPU_ERR_ARG, "mesh %u: %u vertices / %u indices out of range", id, nverts, nidx);
    h.mesh_id = id;
    h.reserved = 0;
    h.nverts = nverts;
    h.nidx = nidx;
    st = link_send(&t->l, T_MESH, &h, sizeof h, v, (size_t)nverts * sizeof *v, idx, (size_t)nidx * 2u);
    if (st < 0)
        return st;
    {
        int32_t s;
        return reply_fixed(&t->l, T_MESH, &s, sizeof s, t->l.timeout_ms);
    }
}

int teensy_frame_send(teensy_conn *t, const geom_frame_hdr *hdr, const geom_draw *draws)
{
    int st = t_ready(t);
    if (st < 0)
        return st;
    if (hdr->ndraws > T_MAX_DRAWS || (hdr->ndraws && !draws))
        return lfail(&t->l, FGPU_ERR_ARG, "T_FRAME with %u draws (max %u)", hdr->ndraws, T_MAX_DRAWS);
    st = link_send(&t->l, T_FRAME, hdr, sizeof *hdr, draws, (size_t)hdr->ndraws * sizeof *draws, NULL, 0);
    if (st < 0)
        return st;
    t->frame_pending = 1;
    return 0;
}

int teensy_frame_recv(teensy_conn *t, t_frame_reply *out, uint32_t (**recs)[GPU_REC_WORDS])
{
    t_frame_reply r;
    uint32_t len, left, n;
    int st;
    if (recs)
        *recs = NULL;
    if (!t->frame_pending)
        return lfail(&t->l, FGPU_ERR_STATE, "no T_FRAME outstanding");
    st = reply_hdr(&t->l, T_FRAME, &len, t->l.timeout_ms > T_SLOW_TIMEOUT ? t->l.timeout_ms : T_SLOW_TIMEOUT);
    if (st < 0)
        return st;
    t->frame_pending = 0;
    left = len;
    st = reply_part(&t->l, &left, &r, sizeof r);
    if (st < 0)
        return st;
    if (out)
        *out = r;
    if (len < 4)
        return lfail(&t->l, FGPU_ERR_REPLY, "T_FRAME reply without a status");
    n = r.nrecs_returned;
    if ((uint64_t)n * GPU_REC_BYTES != left) {
        link_drain(&t->l, left);
        return lfail(&t->l, FGPU_ERR_REPLY, "T_FRAME reply: %u records announced, %u payload bytes follow", n, left);
    }
    if (n) {
        if (recs) {
            uint32_t (*buf)[GPU_REC_WORDS] = malloc((size_t)n * GPU_REC_BYTES);
            if (!buf) {
                link_drain(&t->l, left);
                return lfail(&t->l, FGPU_ERR_NOMEM, "no memory for %u returned records", n);
            }
            st = reply_part(&t->l, &left, buf, n * GPU_REC_BYTES);
            if (st < 0) {
                free(buf);
                return st;
            }
            *recs = buf;
        } else {
            st = link_drain(&t->l, left);
            if (st < 0)
                return st;
        }
    }
    return r.status;
}

int teensy_frame(teensy_conn *t, const geom_frame_hdr *hdr, const geom_draw *draws, t_frame_reply *r,
                 uint32_t (**recs)[GPU_REC_WORDS])
{
    int st = teensy_frame_send(t, hdr, draws);
    if (st < 0) {
        if (recs)
            *recs = NULL;
        return st;
    }
    return teensy_frame_recv(t, r, recs);
}

int teensy_records(teensy_conn *t, const uint32_t (*recs)[GPU_REC_WORDS], uint32_t n, uint32_t *sent)
{
    uint32_t total = 0;
    int st = t_ready(t);
    if (sent)
        *sent = 0;
    if (st < 0)
        return st;
    while (n > 0) {
        uint32_t k = n < T_RECS_CHUNK ? n : T_RECS_CHUNK;
        int32_t r[2];
        st = link_send(&t->l, T_RECORDS, &k, 4, recs, (size_t)k * GPU_REC_BYTES, NULL, 0);
        if (st < 0)
            return st;
        st = reply_fixed(&t->l, T_RECORDS, r, sizeof r,
                         t->l.timeout_ms > T_SLOW_TIMEOUT ? t->l.timeout_ms : T_SLOW_TIMEOUT);
        if (st <= -100)
            break;                      /* local failure: nothing is known about this chunk */
        total += (uint32_t)r[1];        /* records of this chunk that went onto the bus */
        if (st < 0)
            break;
        recs += k;
        n -= k;
    }
    if (sent)
        *sent = total;
    return st;
}

int teensy_stats(teensy_conn *t, t_stats_reply *out)
{
    t_stats_reply r;
    int st = t_ready(t);
    if (st < 0)
        return st;
    st = link_send(&t->l, T_STATS, NULL, 0, NULL, 0, NULL, 0);
    if (st < 0)
        return st;
    st = reply_fixed(&t->l, T_STATS, &r, sizeof r, t->l.timeout_ms);
    if (out)
        *out = r;
    return st;
}

int teensy_bus_mode(teensy_conn *t, uint32_t mode)
{
    int st = t_ready(t);
    if (st < 0)
        return st;
    return link_status_call(&t->l, T_BUS_MODE, &mode, 4, t->l.timeout_ms);
}

int teensy_reset(teensy_conn *t)
{
    int st = t_ready(t);
    if (st < 0)
        return st;
    return link_status_call(&t->l, T_RESET, NULL, 0, t->l.timeout_ms);
}

int teensy_scene(teensy_conn *t, const t_scene_hdr *h, const t_scene_obj *objs,
                 const uint32_t (*overlay)[GPU_REC_WORDS])
{
    int st = t_ready(t);
    int32_t s;
    if (st < 0)
        return st;
    if (h->nobjs > T_MAX_SCENE_OBJS || h->noverlay > T_MAX_OVERLAY_RECS || (h->nobjs && !objs) ||
        (h->noverlay && !overlay))
        return lfail(&t->l, FGPU_ERR_ARG, "T_SCENE: %u objects / %u overlay records (max %u / %u)", h->nobjs,
                     h->noverlay, T_MAX_SCENE_OBJS, T_MAX_OVERLAY_RECS);
    st = link_send(&t->l, T_SCENE, h, sizeof *h, objs, (size_t)h->nobjs * sizeof *objs, overlay,
                   (size_t)h->noverlay * GPU_REC_BYTES);
    if (st < 0)
        return st;
    return reply_fixed(&t->l, T_SCENE, &s, sizeof s, t->l.timeout_ms);
}

int teensy_auto(teensy_conn *t, uint32_t enable, uint32_t max_fps)
{
    t_auto a;
    int st = t_ready(t);
    if (st < 0)
        return st;
    a.enable = enable;
    a.max_fps = max_fps;
    return link_status_call(&t->l, T_AUTO, &a, sizeof a, t->l.timeout_ms);
}
