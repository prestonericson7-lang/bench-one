/* ===========================================================================================
 *  stage_link.c -- see stage_link.h
 * ===========================================================================================
 */

#include "stage_link.h"
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
  #include <windows.h>
  #define STAGE_ERRNO WSAGetLastError()
#else
  #include <errno.h>
  #include <time.h>
  #define STAGE_ERRNO errno
  #define closesocket close
#endif

/* --- little-endian field access, so two different compilers agree --------------------------- */

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;         p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t get64(const uint8_t *p) { return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32); }

void stage_hdr_pack(const stage_hdr_t *h, uint8_t out[STAGE_HDR_BYTES])
{
    put32(out + 0,  STAGE_MAGIC);
    put16(out + 4,  STAGE_VERSION);
    put16(out + 6,  h->type);
    put32(out + 8,  h->token_id);
    put16(out + 12, h->stage);
    put16(out + 14, h->layer);
    put32(out + 16, h->len);
    put32(out + 20, h->seq);
    put64(out + 24, h->t_emit_us);
    put32(out + 32, h->stream);
    put32(out + 36, h->reserved);
}

int stage_hdr_unpack(stage_hdr_t *h, const uint8_t in[STAGE_HDR_BYTES])
{
    h->magic = get32(in + 0);
    if (h->magic != STAGE_MAGIC) return -1;
    h->version = get16(in + 4);
    if (h->version != STAGE_VERSION) return -2;
    h->type      = get16(in + 6);
    h->token_id  = get32(in + 8);
    h->stage     = get16(in + 12);
    h->layer     = get16(in + 14);
    h->len       = get32(in + 16);
    /* A length the receiver cannot hold is rejected HERE, before any allocation or read. A header
     * arriving from a mis-wired peer is the ordinary case, not the exotic one. */
    if (h->len > STAGE_MAX_PAYLOAD) return -3;
    h->seq       = get32(in + 20);
    h->t_emit_us = get64(in + 24);
    h->stream    = get32(in + 32);
    h->reserved  = get32(in + 36);
    return 0;
}

/* --- lifecycle ----------------------------------------------------------------------------- */

int stage_init(void)
{
#ifdef _WIN32
    WSADATA w;
    return WSAStartup(MAKEWORD(2, 2), &w) == 0 ? 0 : -1;
#else
    return 0;
#endif
}

void stage_shutdown(void)
{
#ifdef _WIN32
    WSACleanup();
#endif
}

uint64_t stage_now_us(void)
{
#ifdef _WIN32
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (uint64_t)((double)t.QuadPart / (double)f.QuadPart * 1e6);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
#endif
}

int stage_nodelay(stage_fd_t fd)
{
    int one = 1;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
}

stage_fd_t stage_listen(uint16_t port)
{
    stage_fd_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == STAGE_BAD_FD) return STAGE_BAD_FD;

    /* Without SO_REUSEADDR a stage that just exited leaves the port in TIME_WAIT and the restart
     * fails for a couple of minutes -- which during bring-up is every single restart. */
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);

    if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0) { closesocket(s); return STAGE_BAD_FD; }
    if (listen(s, 4) != 0) { closesocket(s); return STAGE_BAD_FD; }
    return s;
}

stage_fd_t stage_accept(stage_fd_t server)
{
    stage_fd_t c = accept(server, NULL, NULL);
    if (c != STAGE_BAD_FD) stage_nodelay(c);
    return c;
}

stage_fd_t stage_connect(const char *host, uint16_t port)
{
    stage_fd_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == STAGE_BAD_FD) return STAGE_BAD_FD;

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) { closesocket(s); return STAGE_BAD_FD; }

    if (connect(s, (struct sockaddr *)&a, sizeof(a)) != 0) { closesocket(s); return STAGE_BAD_FD; }
    stage_nodelay(s);
    return s;
}

void stage_close(stage_fd_t fd)
{
    if (fd != STAGE_BAD_FD) closesocket(fd);
}

/* --- whole-message transfer ---------------------------------------------------------------- */

static int send_all(stage_fd_t fd, const uint8_t *p, uint32_t n)
{
    uint32_t sent = 0;
    while (sent < n) {
        const int r = send(fd, (const char *)p + sent, (int)(n - sent), 0);
        if (r <= 0) return -1;
        sent += (uint32_t)r;
    }
    return 0;
}

static int recv_all(stage_fd_t fd, uint8_t *p, uint32_t n)
{
    uint32_t got = 0;
    while (got < n) {
        const int r = recv(fd, (char *)p + got, (int)(n - got), 0);
        if (r <= 0) return -1;
        got += (uint32_t)r;
    }
    return 0;
}

int stage_send(stage_fd_t fd, const stage_hdr_t *h, const void *payload)
{
    uint8_t hdr[STAGE_HDR_BYTES];
    stage_hdr_pack(h, hdr);
    if (send_all(fd, hdr, STAGE_HDR_BYTES) != 0) return -1;
    if (h->len && payload) return send_all(fd, (const uint8_t *)payload, h->len);
    return 0;
}

int stage_recv(stage_fd_t fd, stage_hdr_t *h, void *payload, uint32_t max_payload)
{
    uint8_t hdr[STAGE_HDR_BYTES];
    if (recv_all(fd, hdr, STAGE_HDR_BYTES) != 0) return -1;
    const int rc = stage_hdr_unpack(h, hdr);
    if (rc != 0) return rc;
    if (h->len > max_payload) return -4;
    if (h->len && payload) return recv_all(fd, (uint8_t *)payload, h->len);
    return 0;
}
