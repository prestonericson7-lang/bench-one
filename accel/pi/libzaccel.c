/* libzaccel -- see libzaccel.h.  Wire protocol: accel/SPEC.md §4 (TCP, little-endian). */
#define _POSIX_C_SOURCE 200809L

#include "libzaccel.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#ifndef ZACCEL_NO_RESOLVER
#include <netdb.h>
#endif

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "libzaccel reads int32 results straight off the wire: little-endian hosts only"
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define REQ_MAGIC 0x3151415Au   /* "ZAQ1" */
#define REP_MAGIC 0x3152415Au   /* "ZAR1" */
#define OP_INFO 1u
#define OP_LOAD 2u
#define OP_FREE 3u
#define OP_GEMV 4u
#define OP_PING 5u

#define DRAIN_CAP (1u << 20)    /* an error reply carrying more than this is malformed */

struct zaccel {
    int      fd;
    uint32_t seq;
};

/* ---------------------------------------------------------------- little-endian helpers */
static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* ---------------------------------------------------------------- connection */
const char *zaccel_default_host(void)
{
    const char *h = getenv("ZACCEL_HOST");
    return (h && *h) ? h : ZACCEL_DEFAULT_HOST;
}

static int env_timeout_ms(int dflt)
{
    const char *s = getenv("ZACCEL_TIMEOUT_MS");
    if (s && *s) {
        long v = strtol(s, NULL, 10);
        if (v > 0 && v < 3600000) return (int)v;
    }
    return dflt;
}

/* Connect with a deadline, then leave the socket blocking with send/receive timeouts. */
static int connect_addr(const struct sockaddr *sa, socklen_t salen, int conn_ms, int io_ms)
{
    int fd = socket(sa->sa_family, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) goto fail;
    if (connect(fd, sa, salen) < 0) {
        if (errno != EINPROGRESS) goto fail;
        struct pollfd p = { .fd = fd, .events = POLLOUT };
        int n;
        do n = poll(&p, 1, conn_ms); while (n < 0 && errno == EINTR);
        if (n == 0) { errno = ETIMEDOUT; goto fail; }
        if (n < 0) goto fail;
        int err = 0; socklen_t el = sizeof err;
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) < 0) goto fail;
        if (err) { errno = err; goto fail; }
    }
    if (fcntl(fd, F_SETFL, fl) < 0) goto fail;
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    struct timeval tv = { .tv_sec = io_ms / 1000, .tv_usec = (io_ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    return fd;
fail:;
    int e = errno;
    close(fd);
    errno = e;
    return -1;
}

zaccel_t *zaccel_connect(const char *host, int port)
{
    if ((!host || !*host) && !(getenv("ZACCEL_HOST") && *getenv("ZACCEL_HOST"))) {
        /* no address given: the Zynq holds both 10.20.0.2 (car LAN) and 10.77.0.2 (the GPU's
         * direct-cable link), but the Pi may only have a route to one of them */
        zaccel_t *z = zaccel_connect(ZACCEL_DEFAULT_HOST, port);
        return z ? z : zaccel_connect(ZACCEL_ALT_HOST, port);
    }
    if (!host || !*host) host = zaccel_default_host();
    if (port <= 0) port = ZACCEL_DEFAULT_PORT;
    if (port > 65535) { errno = EINVAL; return NULL; }
    int conn_ms = env_timeout_ms(3000), io_ms = env_timeout_ms(60000);
    int fd = -1;

    struct sockaddr_in  a4; memset(&a4, 0, sizeof a4);
    struct sockaddr_in6 a6; memset(&a6, 0, sizeof a6);
    if (inet_pton(AF_INET, host, &a4.sin_addr) == 1) {
        a4.sin_family = AF_INET; a4.sin_port = htons((uint16_t)port);
        fd = connect_addr((struct sockaddr *)&a4, sizeof a4, conn_ms, io_ms);
    } else if (inet_pton(AF_INET6, host, &a6.sin6_addr) == 1) {
        a6.sin6_family = AF_INET6; a6.sin6_port = htons((uint16_t)port);
        fd = connect_addr((struct sockaddr *)&a6, sizeof a6, conn_ms, io_ms);
    } else {
#ifdef ZACCEL_NO_RESOLVER
        /* Static builds: glibc's resolver needs its own shared libraries at run time. */
        errno = EINVAL;
        return NULL;
#else
        struct addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        char ps[8];
        snprintf(ps, sizeof ps, "%d", port);
        int g = getaddrinfo(host, ps, &hints, &res);
        if (g != 0) { errno = EHOSTUNREACH; return NULL; }
        int e = EHOSTUNREACH;
        for (struct addrinfo *ai = res; ai && fd < 0; ai = ai->ai_next) {
            fd = connect_addr(ai->ai_addr, ai->ai_addrlen, conn_ms, io_ms);
            if (fd < 0) e = errno;
        }
        freeaddrinfo(res);
        if (fd < 0) errno = e;
#endif
    }
    if (fd < 0) return NULL;
    zaccel_t *z = calloc(1, sizeof *z);
    if (!z) { close(fd); errno = ENOMEM; return NULL; }
    z->fd = fd;
    z->seq = 1;
    return z;
}

void zaccel_close(zaccel_t *z)
{
    if (!z) return;
    if (z->fd >= 0) close(z->fd);
    free(z);
}

static int kill_conn(zaccel_t *z, int rc)
{
    if (z->fd >= 0) close(z->fd);
    z->fd = -1;
    return rc;
}

/* ---------------------------------------------------------------- raw I/O */
static int send_iov(zaccel_t *z, struct iovec *iov, int n)
{
    while (n > 0) {
        struct msghdr m;
        memset(&m, 0, sizeof m);
        m.msg_iov = iov;
        m.msg_iovlen = (size_t)n;
        ssize_t w = sendmsg(z->fd, &m, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR) continue;
            return kill_conn(z, ZACCEL_E_IO);
        }
        size_t left = (size_t)w;
        while (n > 0 && left >= iov->iov_len) { left -= iov->iov_len; iov++; n--; }
        if (n > 0) { iov->iov_base = (uint8_t *)iov->iov_base + left; iov->iov_len -= left; }
    }
    return ZACCEL_OK;
}

static int recv_all(zaccel_t *z, void *buf, size_t len)
{
    uint8_t *p = buf;
    while (len) {
        ssize_t r = recv(z->fd, p, len, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return kill_conn(z, ZACCEL_E_IO);
        p += r; len -= (size_t)r;
    }
    return ZACCEL_OK;
}

static int drain(zaccel_t *z, uint32_t len)
{
    uint8_t tmp[4096];
    if (len > DRAIN_CAP) return kill_conn(z, ZACCEL_E_PROTO);
    while (len) {
        uint32_t n = len < sizeof tmp ? len : (uint32_t)sizeof tmp;
        int rc = recv_all(z, tmp, n);
        if (rc) return rc;
        len -= n;
    }
    return ZACCEL_OK;
}

/* One transaction: send op + payload (iov[1..n-1]; iov[0] is reserved for the header), read the
 * reply header.  Returns <0 local error, 0 OK with *rlen set and the payload still unread, >0 the
 * server status with the payload already drained. */
static int xact(zaccel_t *z, uint32_t op, struct iovec *iov, int n, uint32_t *rlen)
{
    if (!z) return ZACCEL_E_ARG;
    if (z->fd < 0) return ZACCEL_E_CLOSED;
    uint64_t len = 0;
    for (int i = 1; i < n; i++) len += iov[i].iov_len;
    if (len > 0xFFFFFFFFu) return ZACCEL_E_ARG;
    uint32_t seq = z->seq++;
    uint8_t h[16];
    put_u32(h, REQ_MAGIC); put_u32(h + 4, op); put_u32(h + 8, seq); put_u32(h + 12, (uint32_t)len);
    iov[0].iov_base = h;
    iov[0].iov_len = sizeof h;
    int rc = send_iov(z, iov, n);
    if (rc) return rc;
    uint8_t r[16];
    rc = recv_all(z, r, sizeof r);
    if (rc) return rc;
    if (get_u32(r) != REP_MAGIC || get_u32(r + 8) != seq) return kill_conn(z, ZACCEL_E_PROTO);
    uint32_t status = get_u32(r + 4);
    *rlen = get_u32(r + 12);
    if (status != 0) {
        rc = drain(z, *rlen);
        if (rc) return rc;
        return status > 0x7FFFFFFFu ? kill_conn(z, ZACCEL_E_PROTO) : (int)status;
    }
    return ZACCEL_OK;
}

/* ---------------------------------------------------------------- operations */
int zaccel_info(zaccel_t *z, zaccel_info_t *out)
{
    struct iovec iov[1];
    uint32_t rlen;
    int rc = xact(z, OP_INFO, iov, 1, &rlen);
    if (rc) return rc;
    if (rlen < 28 || rlen > 4096) return kill_conn(z, ZACCEL_E_PROTO);
    uint8_t b[4096];
    rc = recv_all(z, b, rlen);
    if (rc) return rc;
    if (out) {
        out->version = get_u32(b);        out->engine = get_u32(b + 4);
        out->mem_total_mb = get_u32(b + 8); out->mem_free_mb = get_u32(b + 12);
        out->max_cols = get_u32(b + 16);  out->max_batch = get_u32(b + 20);
        out->selftest = get_u32(b + 24);
    }
    return ZACCEL_OK;
}

int zaccel_ping(zaccel_t *z, const void *data, uint32_t len)
{
    if (len && !data) return ZACCEL_E_ARG;
    struct iovec iov[2] = { { 0 }, { (void *)(uintptr_t)data, len } };
    uint32_t rlen;
    int rc = xact(z, OP_PING, iov, len ? 2 : 1, &rlen);
    if (rc) return rc;
    if (rlen != len) return kill_conn(z, ZACCEL_E_PROTO);
    if (!len) return ZACCEL_OK;
    uint8_t *b = malloc(len);
    if (!b) return kill_conn(z, ZACCEL_E_NOMEM);
    rc = recv_all(z, b, len);
    if (rc == ZACCEL_OK && memcmp(b, data, len) != 0) rc = ZACCEL_E_PROTO;
    free(b);
    return rc;
}

int zaccel_load(zaccel_t *z, uint32_t mode, uint32_t rows, uint32_t cols,
                const uint8_t *packed, zaccel_tensor_t *t)
{
    size_t rb = zaccel_row_bytes(mode, cols);
    if (!rb || !rows || !packed || !t) return ZACCEL_E_ARG;
    uint64_t bytes = (uint64_t)rows * rb;
    if (bytes + 12 > 0xFFFFFFFFu) return ZACCEL_E_ARG;
    uint8_t p[12];
    put_u32(p, mode); put_u32(p + 4, rows); put_u32(p + 8, cols);
    struct iovec iov[3] = { { 0 }, { p, sizeof p }, { (void *)(uintptr_t)packed, (size_t)bytes } };
    uint32_t rlen;
    int rc = xact(z, OP_LOAD, iov, 3, &rlen);
    if (rc) return rc;
    if (rlen != 4) return kill_conn(z, ZACCEL_E_PROTO);
    uint8_t b[4];
    rc = recv_all(z, b, 4);
    if (rc) return rc;
    t->id = get_u32(b); t->mode = mode; t->rows = rows; t->cols = cols;
    return ZACCEL_OK;
}

int zaccel_free(zaccel_t *z, const zaccel_tensor_t *t)
{
    if (!t) return ZACCEL_E_ARG;
    uint8_t p[4];
    put_u32(p, t->id);
    struct iovec iov[2] = { { 0 }, { p, sizeof p } };
    uint32_t rlen;
    int rc = xact(z, OP_FREE, iov, 2, &rlen);
    if (rc) return rc;
    return drain(z, rlen);
}

int zaccel_gemv(zaccel_t *z, const zaccel_tensor_t *t, uint32_t nb, const int8_t *A,
                int32_t *Y, uint32_t *cycles, uint32_t *engine_used)
{
    if (!t || !A || !Y || nb == 0 || t->rows == 0 || t->cols == 0) return ZACCEL_E_ARG;
    uint64_t alen = (uint64_t)nb * t->cols, ylen = (uint64_t)t->rows * nb * 4;
    if (alen + 8 > 0xFFFFFFFFu || ylen + 8 > 0xFFFFFFFFu) return ZACCEL_E_ARG;
    uint8_t p[8];
    put_u32(p, t->id); put_u32(p + 4, nb);
    struct iovec iov[3] = { { 0 }, { p, sizeof p }, { (void *)(uintptr_t)A, (size_t)alen } };
    uint32_t rlen;
    int rc = xact(z, OP_GEMV, iov, 3, &rlen);
    if (rc) return rc;
    if (rlen != ylen + 8) return kill_conn(z, ZACCEL_E_PROTO);
    uint8_t h[8];
    rc = recv_all(z, h, 8);
    if (rc) return rc;
    rc = recv_all(z, Y, (size_t)ylen);
    if (rc) return rc;
    if (cycles) *cycles = get_u32(h);
    if (engine_used) *engine_used = get_u32(h + 4);
    return ZACCEL_OK;
}

const char *zaccel_strerror(int rc)
{
    switch (rc) {
    case ZACCEL_OK:          return "ok";
    case ZACCEL_ST_BADREQ:   return "server: bad request";
    case ZACCEL_ST_NOMEM:    return "server: no memory";
    case ZACCEL_ST_ENGINE:   return "server: engine error";
    case ZACCEL_ST_NOTENSOR: return "server: unknown tensor";
    case ZACCEL_E_IO:        return "socket error or timeout (connection closed)";
    case ZACCEL_E_PROTO:     return "malformed reply (connection closed)";
    case ZACCEL_E_ARG:       return "bad argument";
    case ZACCEL_E_NOMEM:     return "out of local memory";
    case ZACCEL_E_CLOSED:    return "connection already closed";
    default:                 return rc > 0 ? "server: unknown status" : "unknown error";
    }
}

/* ---------------------------------------------------------------- packing and the reference */
size_t zaccel_row_bytes(uint32_t mode, uint32_t cols)
{
    if (mode == ZACCEL_MODE_INT4) return 8 * (((size_t)cols + 15) / 16);
    if (mode == ZACCEL_MODE_INT8) return 8 * (((size_t)cols + 7) / 8);
    return 0;
}

int zaccel_pack_int4(const int8_t *w, uint32_t rows, uint32_t cols, uint8_t *out)
{
    size_t rb = zaccel_row_bytes(ZACCEL_MODE_INT4, cols);
    int bad = 0;
    for (uint32_t r = 0; r < rows; r++) {
        const int8_t *s = w + (size_t)r * cols;
        uint8_t *d = out + (size_t)r * rb;
        memset(d, 0, rb);
        for (uint32_t k = 0; k < cols; k++) {
            if (s[k] < -8 || s[k] > 7) bad = 1;
            d[k >> 1] |= (uint8_t)(((unsigned)s[k] & 0xFu) << ((k & 1u) * 4));
        }
    }
    return bad ? ZACCEL_E_ARG : ZACCEL_OK;
}

void zaccel_pack_int8(const int8_t *w, uint32_t rows, uint32_t cols, uint8_t *out)
{
    size_t rb = zaccel_row_bytes(ZACCEL_MODE_INT8, cols);
    for (uint32_t r = 0; r < rows; r++) {
        uint8_t *d = out + (size_t)r * rb;
        memcpy(d, w + (size_t)r * cols, cols);
        memset(d + cols, 0, rb - cols);
    }
}

static int weight_at(uint32_t mode, const uint8_t *row, uint32_t k)
{
    if (mode == ZACCEL_MODE_INT4) {
        int n = (row[k >> 1] >> ((k & 1u) * 4)) & 0xF;
        return n < 8 ? n : n - 16;
    }
    return row[k] < 128 ? row[k] : (int)row[k] - 256;
}

void zaccel_ref_gemv(uint32_t mode, uint32_t rows, uint32_t cols, const uint8_t *packed,
                     uint32_t nb, const int8_t *A, int32_t *Y)
{
    size_t rb = zaccel_row_bytes(mode, cols);
    for (uint32_t r = 0; r < rows; r++) {
        const uint8_t *row = packed + (size_t)r * rb;
        for (uint32_t v = 0; v < nb; v++) {
            const int8_t *a = A + (size_t)v * cols;
            int64_t acc = 0;
            for (uint32_t k = 0; k < cols; k++) acc += (int64_t)weight_at(mode, row, k) * a[k];
            Y[(size_t)r * nb + v] = (int32_t)acc;
        }
    }
}
