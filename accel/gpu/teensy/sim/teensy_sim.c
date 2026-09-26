/*
 * teensy_sim.c -- x86 (Linux / WSL) simulator of the Teensy 4.1 geometry engine (SPEC 12, 13.3, 14).
 *
 * Runs the SAME portable firmware core as the Teensy (teensy_gpu/tg_core.c: T_* message handler,
 * bus safety state machine, autonomous mode, statistics) + common/geom.c + common/gpu_setup.c,
 * on this platform layer (tg_plat.h) instead of teensy_gpu/tg_teensy.cpp:
 *
 *   host link  TCP server (default 127.0.0.1:7779), one client at a time, carrying the Pi <-> Teensy
 *              protocol byte for byte as over USB CDC. A new connection replaces the old one and
 *              drops any half-received message (tg_host_reset, like a DTR edge on the Teensy).
 *   bus        TCP client to the daemon simulator's Teensy bus (default 127.0.0.1:7778); each record
 *              is written as its 96 raw bytes (little-endian words, word 0 first). While
 *              disconnected, a non-blocking connect is retried every 250 ms.
 *              BUSY = 1 while not connected, or while more than 16 KB sent are still queued in the
 *              kernel (SIOCOUTQ) or the socket has no room (poll POLLOUT): the daemon's FIFO
 *              back-pressure (it stops reading) reaches the core exactly like the FPGA's BUSY pin,
 *              and a record that is started always fits (never blocks halfway). So the core's
 *              arming (BUSY = 0 for >= 10 ms) and 500 ms timeout rules run unchanged.
 *   time       CLOCK_MONOTONIC in microseconds (wraps at 2^32 like micros()).
 *   autonomous the core's own loop, driven from this event loop with the wall clock: tg_poll() runs
 *              back to back while tg_wants_cpu() says a frame is due, otherwise the loop sleeps in
 *              poll() for at most 1 ms (so max_fps pacing is accurate to ~1 ms).
 *
 * Options: --port N (7779)  --bind IP (127.0.0.1)  --bus-host IP (127.0.0.1)  --bus-port N (7778;
 *          0 = no bus, BUSY stays 1)  --cpu-mhz N (600, reported in T_STATS)  -v (log link events)
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/sockios.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "tg_common.h"

#define OUTBUF_BYTES   65536u
#define BUS_RETRY_US   250000u
#define BUS_SNDBUF     65536         /* bus socket send buffer (the kernel doubles it) */
#define BUS_QUEUE_MAX  16384         /* BUSY = 1 while more unsent/unacked bytes than this */
#define SEND_STALL_MS  5000          /* a peer that accepts nothing for this long is dropped */

static struct {
    int         port, bus_port, verbose;
    uint32_t    cpu_mhz;
    const char *bind_ip, *bus_host;
} opt = {7779, 7778, 0, 600, "127.0.0.1", "127.0.0.1"};

static int      g_lfd = -1;          /* host listener */
static int      g_cfd = -1;          /* host client */
static int      g_bfd = -1;          /* bus socket */
static int      g_bus_conn;          /* 0 none, 1 connecting, 2 connected */
static uint32_t g_bus_last_try;
static int      g_bus_tried;
static int      g_driving;
static uint8_t  g_out[OUTBUF_BYTES];
static uint32_t g_outn;
static volatile sig_atomic_t g_quit;

static void logf_(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "teensy_sim: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

static void set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0)
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void set_nodelay(int fd)
{
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
}

/* Send all n bytes on a non-blocking socket, waiting for room. 0 = ok, -1 = peer gone/stalled. */
static int send_all(int fd, const uint8_t *p, uint32_t n)
{
    int stalled_ms = 0;
    while (n) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w > 0) {
            p += w;
            n -= (uint32_t)w;
            stalled_ms = 0;
            continue;
        }
        if (w < 0 && errno == EINTR)
            continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pf = {fd, POLLOUT, 0};
            int r = poll(&pf, 1, 100);
            if (r == 0 && (stalled_ms += 100) >= SEND_STALL_MS)
                return -1;
            if (r > 0 && (pf.revents & (POLLERR | POLLHUP | POLLNVAL)))
                return -1;
            continue;
        }
        return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------------------------------- */
/* platform layer (tg_plat.h)                                                                */
/* ---------------------------------------------------------------------------------------- */

uint32_t tgp_micros(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u);
}

uint32_t tgp_cpu_mhz(void) { return opt.cpu_mhz; }

static void client_close(const char *why)
{
    if (g_cfd < 0)
        return;
    if (opt.verbose)
        logf_("host client %s", why);
    close(g_cfd);
    g_cfd = -1;
    g_outn = 0;
}

int tgp_usb_read(void *buf, uint32_t max)
{
    ssize_t r;
    if (g_cfd < 0)
        return 0;
    r = recv(g_cfd, buf, max, MSG_DONTWAIT);
    if (r > 0)
        return (int)r;
    if (r == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
        client_close("disconnected");
    return 0;
}

void tgp_usb_flush(void)
{
    if (g_cfd >= 0 && g_outn && send_all(g_cfd, g_out, g_outn) < 0)
        client_close("lost while sending");
    g_outn = 0;
}

void tgp_usb_write(const void *buf, uint32_t n)
{
    const uint8_t *p = (const uint8_t *)buf;
    if (g_cfd < 0)
        return;                                     /* like USB with no host: dropped */
    while (n) {
        uint32_t m = OUTBUF_BYTES - g_outn;
        if (m > n)
            m = n;
        memcpy(g_out + g_outn, p, m);
        g_outn += m;
        p += m;
        n -= m;
        if (g_outn == OUTBUF_BYTES)
            tgp_usb_flush();
        if (g_cfd < 0)
            return;
    }
}

static void bus_close(const char *why)
{
    if (g_bfd >= 0) {
        if (opt.verbose || g_bus_conn == 2)
            logf_("bus %s:%d %s", opt.bus_host, opt.bus_port, why);
        close(g_bfd);
    }
    g_bfd = -1;
    g_bus_conn = 0;
}

static void bus_try_connect(void)
{
    struct sockaddr_in a;
    uint32_t now = tgp_micros();
    if (opt.bus_port == 0 || g_bus_conn != 0)
        return;
    if (g_bus_tried && now - g_bus_last_try < BUS_RETRY_US)
        return;
    g_bus_tried = 1;
    g_bus_last_try = now;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)opt.bus_port);
    if (inet_pton(AF_INET, opt.bus_host, &a.sin_addr) != 1)
        return;
    g_bfd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_bfd < 0)
        return;
    {   /* bounded send buffer: back-pressure close to the real FIFO (few queued frames) */
        int sb = BUS_SNDBUF;
        setsockopt(g_bfd, SOL_SOCKET, SO_SNDBUF, &sb, sizeof sb);
    }
    set_nonblock(g_bfd);
    set_nodelay(g_bfd);
    if (connect(g_bfd, (struct sockaddr *)&a, sizeof a) == 0) {
        g_bus_conn = 2;
        logf_("bus connected to %s:%d", opt.bus_host, opt.bus_port);
    } else if (errno == EINPROGRESS) {
        g_bus_conn = 1;
    } else {
        close(g_bfd);
        g_bfd = -1;
    }
}

/* Advance the bus connection; returns 1 if connected. */
static int bus_check(void)
{
    struct pollfd pf;
    bus_try_connect();
    if (g_bus_conn == 0)
        return 0;
    pf.fd = g_bfd;
    pf.events = g_bus_conn == 1 ? POLLOUT : POLLIN;
    pf.revents = 0;
    if (poll(&pf, 1, 0) <= 0)
        return g_bus_conn == 2;
    if (g_bus_conn == 1) {
        int err = 0;
        socklen_t l = sizeof err;
        if (getsockopt(g_bfd, SOL_SOCKET, SO_ERROR, &err, &l) < 0 || err != 0) {
            close(g_bfd);                           /* refused: try again later, quietly */
            g_bfd = -1;
            g_bus_conn = 0;
            return 0;
        }
        g_bus_conn = 2;
        logf_("bus connected to %s:%d", opt.bus_host, opt.bus_port);
        return 1;
    }
    if (pf.revents & (POLLIN | POLLERR | POLLHUP)) {  /* the daemon never sends: EOF or error */
        uint8_t junk[256];
        ssize_t r = recv(g_bfd, junk, sizeof junk, MSG_DONTWAIT);
        if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            bus_close("closed by the peer");
            return 0;
        }
    }
    return 1;
}

int tgp_bus_busy(void)
{
    struct pollfd pf;
    int queued = 0;
    if (!bus_check())
        return 1;                                   /* like an absent FPGA (pull-up) */
    /* like the FIFO's "free >= 64 entries": BUSY while more than BUS_QUEUE_MAX bytes wait in the
     * kernel (not yet taken by the daemon), so a record that is started is never blocked halfway */
    if (ioctl(g_bfd, SIOCOUTQ, &queued) == 0 && queued > BUS_QUEUE_MAX)
        return 1;
    pf.fd = g_bfd;
    pf.events = POLLOUT;
    pf.revents = 0;
    return (poll(&pf, 1, 0) == 1 && (pf.revents & POLLOUT)) ? 0 : 1;
}

int tgp_bus_drive(int enable)
{
    if (enable != g_driving && opt.verbose)
        logf_("bus pins %s", enable ? "DRIVEN (bus enabled)" : "high-Z");
    g_driving = enable ? 1 : 0;
    return g_driving;
}

int tgp_bus_send(const uint32_t rec[24])
{
    uint8_t b[GPU_REC_BYTES];
    int i;
    if (!g_driving || g_bus_conn != 2)
        return -1;
    for (i = 0; i < GPU_REC_WORDS; i++) {           /* explicit little-endian */
        b[i * 4 + 0] = (uint8_t)rec[i];
        b[i * 4 + 1] = (uint8_t)(rec[i] >> 8);
        b[i * 4 + 2] = (uint8_t)(rec[i] >> 16);
        b[i * 4 + 3] = (uint8_t)(rec[i] >> 24);
    }
    if (send_all(g_bfd, b, sizeof b) < 0) {
        bus_close("lost while sending");
        return -1;
    }
    return 0;
}

void tgp_led(int on) { (void)on; }

static void wait_events(long timeout_us, int want_bus_out)
{
    struct pollfd pf[3];
    int n = 0;
    if (g_lfd >= 0) {
        pf[n].fd = g_lfd;
        pf[n].events = POLLIN;
        n++;
    }
    if (g_cfd >= 0) {
        pf[n].fd = g_cfd;
        pf[n].events = POLLIN;
        n++;
    }
    if (g_bfd >= 0) {
        pf[n].fd = g_bfd;
        pf[n].events = (short)((g_bus_conn == 1 || want_bus_out) ? POLLOUT : POLLIN);
        n++;
    }
    {
        struct timespec ts = {timeout_us / 1000000, (timeout_us % 1000000) * 1000};
        ppoll(pf, (nfds_t)n, &ts, NULL);
    }
}

/* busy-wait helper of the core (waiting for BUSY): sleep until a host byte arrives, at most 0.2 ms
 * (BUSY is re-read after that; reconnect attempts and the core's timers keep running) */
void tgp_idle(void) { wait_events(200, 0); }

/* ---------------------------------------------------------------------------------------- */
/* main                                                                                      */
/* ---------------------------------------------------------------------------------------- */

static void accept_client(void)
{
    struct sockaddr_in a;
    socklen_t l = sizeof a;
    int fd = accept(g_lfd, (struct sockaddr *)&a, &l);
    char ip[INET_ADDRSTRLEN] = "?";
    if (fd < 0)
        return;
    set_nonblock(fd);
    set_nodelay(fd);
    inet_ntop(AF_INET, &a.sin_addr, ip, sizeof ip);
    if (g_cfd >= 0)
        client_close("replaced by a new connection");
    g_cfd = fd;
    g_outn = 0;
    tg_host_reset();                                 /* like the DTR edge on the real Teensy */
    if (opt.verbose)
        logf_("host client %s:%u connected", ip, (unsigned)ntohs(a.sin_port));
}

static int listen_on(const char *ip, int port)
{
    struct sockaddr_in a;
    int one = 1, fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1 || bind(fd, (struct sockaddr *)&a, sizeof a) < 0 ||
        listen(fd, 4) < 0) {
        close(fd);
        return -1;
    }
    set_nonblock(fd);
    return fd;
}

static void on_signal(int s) { (void)s; g_quit = 1; }

static int parse_port(const char *s, int allow0, int *out)
{
    char *e;
    long v = strtol(s, &e, 10);
    if (!*s || *e || v < (allow0 ? 0 : 1) || v > 65535)
        return -1;
    *out = (int)v;
    return 0;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: teensy_sim [--port N] [--bind IP] [--bus-host IP] [--bus-port N] [--cpu-mhz N] [-v]\n"
            "  --port N       Pi protocol TCP port (7779)\n"
            "  --bind IP      listen address (127.0.0.1)\n"
            "  --bus-host IP  daemon simulator's Teensy bus host (127.0.0.1)\n"
            "  --bus-port N   daemon simulator's Teensy bus port (7778); 0 = no bus (BUSY stays 1)\n"
            "  --cpu-mhz N    value reported as cpu_mhz in T_STATS (600)\n"
            "  -v             log connections and bus pin state changes\n");
}

int main(int argc, char **argv)
{
    int i, mhz;
    struct sigaction sa;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--port") && v && parse_port(v, 0, &opt.port) == 0)
            i++;
        else if (!strcmp(a, "--bus-port") && v && parse_port(v, 1, &opt.bus_port) == 0)
            i++;
        else if (!strcmp(a, "--bind") && v)
            opt.bind_ip = argv[++i];
        else if (!strcmp(a, "--bus-host") && v)
            opt.bus_host = argv[++i];
        else if (!strcmp(a, "--cpu-mhz") && v && parse_port(v, 0, &mhz) == 0) {
            opt.cpu_mhz = (uint32_t)mhz;
            i++;
        } else if (!strcmp(a, "-v"))
            opt.verbose = 1;
        else {
            usage();
            return 2;
        }
    }

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    g_lfd = listen_on(opt.bind_ip, opt.port);
    if (g_lfd < 0) {
        logf_("cannot listen on %s:%d: %s", opt.bind_ip, opt.port, strerror(errno));
        return 1;
    }
    tg_init();
    logf_("listening on %s:%d, bus -> %s:%d, cpu_mhz %u", opt.bind_ip, opt.port,
          opt.bus_port ? opt.bus_host : "(none)", opt.bus_port, (unsigned)opt.cpu_mhz);

    while (!g_quit) {
        struct pollfd pf = {g_lfd, POLLIN, 0};
        if (poll(&pf, 1, 0) > 0 && (pf.revents & POLLIN))
            accept_client();
        tg_poll();
        if (!tg_wants_cpu())
            wait_events(1000, 0);
    }
    client_close("closing (signal)");
    bus_close("closing (signal)");
    close(g_lfd);
    logf_("exit");
    return 0;
}
