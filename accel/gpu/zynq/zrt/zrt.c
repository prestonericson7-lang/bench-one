/*
 * zrt.c -- freestanding runtime for the static ARMv7-A build of fpgagpud (Zynq-7000, Cortex-A9).
 *
 * Why: the cross toolchain on the build host (arm-linux-gnueabihf-gcc 11, Ubuntu 22.04) has no
 * armhf C library installed (libc6-dev-armhf-cross missing), so a static glibc link is impossible
 * and nothing may be installed. fpgagpud needs very little from a libc, so this file provides it
 * directly on top of Linux ARM EABI system calls:
 *   _start, os.h (sockets, ppoll, clock, mmap2, fork/wait for the probe, logging),
 *   memcpy/memmove/memset/memcmp/str*, a small vsnprintf, an mmap-backed malloc, llrint, raise.
 * Nothing here is ARM-specific except the syscall instruction, _start and dsb; syscall numbers,
 * flag values and struct layouts are from the kernel tree the Zynq runs (6.12,
 * arch/arm/include/generated/uapi/asm/unistd-eabi.h and include/uapi/...).
 *
 * Build: -marm (A32; r7 is the syscall-number register and must not be the Thumb frame pointer),
 * -ffreestanding -nostdlib -static, link with -lgcc (64-bit division, double<->int64 helpers).
 * Tested by running the ARM binary under qemu-arm with the simulated PL (make test-arm).
 */
#define ZRT_IMPL
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include "string.h"
#include "math.h"
#include "../os.h"

#if !defined(__arm__) || defined(__thumb__) || !defined(__ARM_EABI__)
#error "zrt.c is the ARM (A32) EABI Linux runtime -- build it with -marm"
#endif

/* ---- system calls (EABI numbers, __NR_SYSCALL_BASE = 0) ------------------------------------ */
#define NR_read            3
#define NR_write           4
#define NR_close           6
#define NR_getpid          20
#define NR_kill            37
#define NR_setrlimit       75
#define NR_munmap          91
#define NR_wait4           114
#define NR_clone           120
#define NR_mmap2           192
#define NR_exit_group      248
#define NR_rt_sigaction    174
#define NR_clock_gettime   263
#define NR_socket          281
#define NR_bind            282
#define NR_listen          284
#define NR_sendto          290
#define NR_recvfrom        292
#define NR_setsockopt      294
#define NR_openat          322
#define NR_ppoll           336
#define NR_pipe2           359
#define NR_accept4         366

/* flag values (asm-generic unless noted) */
#define Z_O_RDONLY     00000000
#define Z_O_RDWR       00000002
#define Z_O_NONBLOCK   00004000
#define Z_O_SYNC       04010000     /* __O_SYNC | O_DSYNC */
#define Z_O_LARGEFILE  00400000     /* ARM value (arch/arm/include/uapi/asm/fcntl.h) */
#define Z_O_CLOEXEC    02000000
#define Z_AT_FDCWD     (-100)
#define Z_PROT_READ    1
#define Z_PROT_WRITE   2
#define Z_MAP_SHARED   0x01
#define Z_MAP_PRIVATE  0x02
#define Z_MAP_ANON     0x20
#define Z_AF_INET      2
#define Z_SOCK_STREAM  1
#define Z_SOL_SOCKET   1
#define Z_SO_REUSEADDR 2
#define Z_SO_SNDBUF    7
#define Z_SO_RCVBUF    8
#define Z_SO_KEEPALIVE 9
#define Z_SO_SNDBUFFORCE 32
#define Z_SO_RCVBUFFORCE 33
#define Z_IPPROTO_TCP  6
#define Z_TCP_NODELAY  1
#define Z_TCP_KEEPIDLE 4
#define Z_TCP_KEEPINTVL 5
#define Z_TCP_KEEPCNT  6
#define Z_MSG_DONTWAIT 0x40
#define Z_MSG_NOSIGNAL 0x4000
#define Z_SIGCHLD      17
#define Z_SIGPIPE      13
#define Z_RLIMIT_CORE  4
#define Z_WNOHANG      1
#define Z_CLOCK_MONOTONIC 1

static long sc(long n, long a, long b, long c, long d, long e, long f)
{
    register long r0 __asm__("r0") = a;
    register long r1 __asm__("r1") = b;
    register long r2 __asm__("r2") = c;
    register long r3 __asm__("r3") = d;
    register long r4 __asm__("r4") = e;
    register long r5 __asm__("r5") = f;
    register long r7 __asm__("r7") = n;
    __asm__ volatile("svc #0"
                     : "+r"(r0)
                     : "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5), "r"(r7)
                     : "memory", "cc");
    return r0;
}
#define SC0(n)                 sc((n), 0, 0, 0, 0, 0, 0)
#define SC1(n, a)              sc((n), (long)(a), 0, 0, 0, 0, 0)
#define SC2(n, a, b)           sc((n), (long)(a), (long)(b), 0, 0, 0, 0)
#define SC3(n, a, b, c)        sc((n), (long)(a), (long)(b), (long)(c), 0, 0, 0)
#define SC4(n, a, b, c, d)     sc((n), (long)(a), (long)(b), (long)(c), (long)(d), 0, 0)
#define SC5(n, a, b, c, d, e)  sc((n), (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), 0)
#define SC6(n, a, b, c, d, e, f) sc((n), (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), (long)(f))

static int is_err(long r) { return (unsigned long)r >= (unsigned long)-4095L; }

/* ---- process entry ---------------------------------------------------------------------- */
int main(int argc, char **argv);
void zrt_cstart(int argc, char **argv) __attribute__((noreturn, used));

__asm__(
    "    .text\n"
    "    .arm\n"
    "    .align 2\n"
    "    .global _start\n"
    "    .type _start, %function\n"
    "_start:\n"
    "    mov fp, #0\n"
    "    mov lr, #0\n"
    "    ldr r0, [sp]\n"          /* argc */
    "    add r1, sp, #4\n"        /* argv */
    "    bic sp, sp, #7\n"        /* AAPCS: 8-byte aligned stack at public interfaces */
    "    bl zrt_cstart\n"
    "1:  b 1b\n"
    "    .size _start, .-_start\n");

void zrt_cstart(int argc, char **argv)
{
    os_exit(main(argc, argv));
}

void os_exit(int code)
{
    for (;;)
        SC1(NR_exit_group, code);
}

/* libgcc's integer division-by-zero handler (__aeabi_idiv0/ldiv0) calls raise(SIGFPE) */
int raise(int sig);
int raise(int sig)
{
    SC2(NR_kill, SC0(NR_getpid), sig);
    return 0;
}

/* ---- memory and strings ---------------------------------------------------------------- */
typedef uint32_t __attribute__((may_alias)) u32a;

void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *dp = d;
    const unsigned char *sp = s;
    if ((((uintptr_t)dp | (uintptr_t)sp) & 3u) == 0) {
        while (n >= 16) {
            ((u32a *)dp)[0] = ((const u32a *)sp)[0];
            ((u32a *)dp)[1] = ((const u32a *)sp)[1];
            ((u32a *)dp)[2] = ((const u32a *)sp)[2];
            ((u32a *)dp)[3] = ((const u32a *)sp)[3];
            dp += 16; sp += 16; n -= 16;
        }
        while (n >= 4) {
            *(u32a *)dp = *(const u32a *)sp;
            dp += 4; sp += 4; n -= 4;
        }
    }
    while (n--)
        *dp++ = *sp++;
    return d;
}

void *memmove(void *d, const void *s, size_t n)
{
    unsigned char *dp = d;
    const unsigned char *sp = s;
    if (dp == sp || n == 0)
        return d;
    if (dp < sp || dp >= sp + n)
        return memcpy(d, s, n);
    while (n--)
        dp[n] = sp[n];
    return d;
}

void *memset(void *d, int c, size_t n)
{
    unsigned char *dp = d;
    uint32_t w = (unsigned char)c;
    w |= w << 8;
    w |= w << 16;
    while (n && ((uintptr_t)dp & 3u)) {
        *dp++ = (unsigned char)c;
        n--;
    }
    while (n >= 4) {
        *(u32a *)dp = w;
        dp += 4;
        n -= 4;
    }
    while (n--)
        *dp++ = (unsigned char)c;
    return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    size_t i;
    for (i = 0; i < n; i++)
        if (x[i] != y[i])
            return x[i] < y[i] ? -1 : 1;
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        if (*a != *b)
            return (unsigned char)*a - (unsigned char)*b;
        if (!*a)
            return 0;
    }
    return 0;
}

char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c)
            return (char *)(uintptr_t)s;
        if (!*s)
            return NULL;
    }
}

/* ---- malloc: every block is its own anonymous mapping (the daemon allocates rarely) ------- */
typedef struct {
    size_t   maplen;
    size_t   size;
    uint32_t magic;
    uint32_t pad;
} mhdr;                                /* 16 bytes: user pointers are 16-byte aligned */
#define MHDR_MAGIC 0x7A72744Du

void *os_malloc(size_t n)
{
    size_t need = n + sizeof(mhdr), maplen;
    long r;
    mhdr *h;
    if (need < n)
        return NULL;
    maplen = (need + 4095u) & ~(size_t)4095u;
    if (maplen < need)
        return NULL;
    r = SC6(NR_mmap2, 0, maplen, Z_PROT_READ | Z_PROT_WRITE, Z_MAP_PRIVATE | Z_MAP_ANON, -1, 0);
    if (is_err(r))
        return NULL;
    h = (mhdr *)r;
    h->maplen = maplen;
    h->size = n;
    h->magic = MHDR_MAGIC;
    return h + 1;
}

void *os_calloc(size_t n)
{
    return os_malloc(n);               /* fresh anonymous pages are zero */
}

void os_free(void *p)
{
    mhdr *h;
    if (!p)
        return;
    h = (mhdr *)p - 1;
    if (h->magic != MHDR_MAGIC)
        return;                        /* not ours: leak rather than corrupt */
    h->magic = 0;
    SC2(NR_munmap, h, h->maplen);
}

void *os_realloc(void *p, size_t n)
{
    mhdr *h;
    void *q;
    if (!p)
        return os_malloc(n);
    h = (mhdr *)p - 1;
    if (n <= h->maplen - sizeof(mhdr)) {
        h->size = n;
        return p;
    }
    q = os_malloc(n);
    if (!q)
        return NULL;
    memcpy(q, p, h->size < n ? h->size : n);
    os_free(p);
    return q;
}

/* ---- llrint (round half to even, as glibc in the default FP environment) --------------- */
long long llrint(double x)
{
    const double two52 = 4503599627370496.0;             /* 2^52 */
    const double two63 = 9223372036854775808.0;          /* 2^63 */
    volatile double t;
    if (!(x > -two63 && x < two63))
        return (long long)(-9223372036854775807LL - 1);  /* NaN / out of range: like x86 glibc */
    if (!(x < two52 && x > -two52))
        return (long long)x;                             /* |x| >= 2^52: already an integer */
    if (x >= 0.0) {
        t = x + two52;                                   /* rounds x to an integer, ties even */
        t = t - two52;                                   /* exact */
    } else {
        t = x - two52;
        t = t + two52;
    }
    return (long long)t;
}

/* ---- formatting ------------------------------------------------------------------------- */
static void put_ch(char *buf, size_t n, size_t *pos, char c)
{
    if (*pos + 1 < n)
        buf[*pos] = c;
    (*pos)++;
}

static size_t fmt_u64(char *out, unsigned long long v, unsigned base, int upper)
{
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[24];
    size_t n = 0, i;
    do {
        tmp[n++] = dig[v % base];
        v /= base;
    } while (v);
    for (i = 0; i < n; i++)
        out[i] = tmp[n - 1 - i];
    return n;
}

/* Supports: flags '-' '0', width (digits or '*'), precision for %s, length l ll z,
 * conversions d i u x X p s c %. Enough for every format string in fpgagpud. */
int os_vsnprintf(char *buf, size_t n, const char *fmt, va_list ap)
{
    size_t pos = 0;
    for (; *fmt; fmt++) {
        int left = 0, zero = 0, width = 0, prec = -1, lng = 0, isz = 0, numeric = 0;
        char num[40], sign = 0;
        const char *s = num;
        size_t len = 0, total, pad, i;

        if (*fmt != '%') {
            put_ch(buf, n, &pos, *fmt);
            continue;
        }
        fmt++;
        for (;; fmt++) {
            if (*fmt == '-')
                left = 1;
            else if (*fmt == '0')
                zero = 1;
            else
                break;
        }
        if (*fmt == '*') {
            width = va_arg(ap, int);
            if (width < 0) {
                left = 1;
                width = -width;
            }
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9')
                width = width * 10 + (*fmt++ - '0');
        }
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') {
                prec = va_arg(ap, int);
                fmt++;
            } else {
                while (*fmt >= '0' && *fmt <= '9')
                    prec = prec * 10 + (*fmt++ - '0');
            }
        }
        while (*fmt == 'l') {
            lng++;
            fmt++;
        }
        if (*fmt == 'z') {
            isz = 1;
            fmt++;
        }
        switch (*fmt) {
        case 'd':
        case 'i': {
            long long v;
            unsigned long long u;
            if (isz)
                v = (long)va_arg(ap, long);
            else if (lng >= 2)
                v = va_arg(ap, long long);
            else if (lng == 1)
                v = va_arg(ap, long);
            else
                v = va_arg(ap, int);
            u = v < 0 ? 0ull - (unsigned long long)v : (unsigned long long)v;
            if (v < 0)
                sign = '-';
            len = fmt_u64(num, u, 10, 0);
            numeric = 1;
            break;
        }
        case 'u':
        case 'x':
        case 'X': {
            unsigned long long u;
            if (isz)
                u = va_arg(ap, size_t);
            else if (lng >= 2)
                u = va_arg(ap, unsigned long long);
            else if (lng == 1)
                u = va_arg(ap, unsigned long);
            else
                u = va_arg(ap, unsigned int);
            len = fmt_u64(num, u, *fmt == 'u' ? 10u : 16u, *fmt == 'X');
            numeric = 1;
            break;
        }
        case 'p': {
            uintptr_t u = (uintptr_t)va_arg(ap, void *);
            num[0] = '0';
            num[1] = 'x';
            len = 2 + fmt_u64(num + 2, u, 16, 0);
            break;
        }
        case 's':
            s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            while (s[len] && (prec < 0 || (int)len < prec))
                len++;
            break;
        case 'c':
            num[0] = (char)va_arg(ap, int);
            len = 1;
            break;
        case '%':
            put_ch(buf, n, &pos, '%');
            continue;
        case 0:
            goto done;
        default:
            put_ch(buf, n, &pos, '%');
            put_ch(buf, n, &pos, *fmt);
            continue;
        }
        total = len + (sign ? 1u : 0u);
        pad = (width > 0 && (size_t)width > total) ? (size_t)width - total : 0;
        if (!left && !(zero && numeric))
            for (; pad; pad--)
                put_ch(buf, n, &pos, ' ');
        if (sign)
            put_ch(buf, n, &pos, sign);
        if (!left && zero && numeric)
            for (; pad; pad--)
                put_ch(buf, n, &pos, '0');
        for (i = 0; i < len; i++)
            put_ch(buf, n, &pos, s[i]);
        for (; pad; pad--)
            put_ch(buf, n, &pos, ' ');
    }
done:
    if (n)
        buf[pos < n ? pos : n - 1] = 0;
    return (int)pos;
}

int os_snprintf(char *buf, size_t n, const char *fmt, ...)
{
    int r;
    va_list ap;
    va_start(ap, fmt);
    r = os_vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

void os_log(const char *fmt, ...)
{
    char buf[1024];
    int n;
    va_list ap;
    memcpy(buf, "fpgagpud: ", 10);
    va_start(ap, fmt);
    n = os_vsnprintf(buf + 10, sizeof buf - 11, fmt, ap);
    va_end(ap);
    if (n < 0)
        n = 0;
    n += 10;
    if (n > (int)sizeof buf - 1)
        n = (int)sizeof buf - 1;
    buf[n++] = '\n';
    SC3(NR_write, 2, buf, n);
}

const char *os_strerror(int e)
{
    static char other[24];
    if (e < 0)
        e = -e;
    switch (e) {
    case 0:   return "Success";
    case 1:   return "Operation not permitted";
    case 2:   return "No such file or directory";
    case 4:   return "Interrupted system call";
    case 5:   return "Input/output error";
    case 9:   return "Bad file descriptor";
    case 11:  return "Resource temporarily unavailable";
    case 12:  return "Cannot allocate memory";
    case 13:  return "Permission denied";
    case 14:  return "Bad address";
    case 16:  return "Device or resource busy";
    case 19:  return "No such device";
    case 22:  return "Invalid argument";
    case 24:  return "Too many open files";
    case 32:  return "Broken pipe";
    case 38:  return "Function not implemented";
    case 92:  return "Protocol not available";
    case 98:  return "Address already in use";
    case 99:  return "Cannot assign requested address";
    case 103: return "Software caused connection abort";
    case 104: return "Connection reset by peer";
    case 110: return "Connection timed out";
    case 111: return "Connection refused";
    case 113: return "No route to host";
    default:
        os_snprintf(other, sizeof other, "errno %d", e);
        return other;
    }
}

/* ---- time ------------------------------------------------------------------------------- */
struct z_timespec32 { long tv_sec; long tv_nsec; };

uint64_t os_now_ns(void)
{
    struct z_timespec32 ts = { 0, 0 };
    SC2(NR_clock_gettime, Z_CLOCK_MONOTONIC, &ts);
    return (uint64_t)(unsigned long)ts.tv_sec * 1000000000ull + (uint64_t)(unsigned long)ts.tv_nsec;
}

/* ---- sockets ---------------------------------------------------------------------------- */
struct z_sockaddr_in {
    uint16_t family;
    uint16_t port;          /* network order */
    uint32_t addr;          /* network order */
    uint8_t  zero[8];
};

static uint16_t z_htons(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }

static int parse_ipv4(const char *s, uint32_t *out_be)
{
    uint32_t parts[4];
    int i;
    for (i = 0; i < 4; i++) {
        uint32_t v = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (uint32_t)(*s++ - '0');
            if (++digits > 3 || v > 255)
                return -1;
        }
        if (!digits)
            return -1;
        parts[i] = v;
        if (i < 3) {
            if (*s != '.')
                return -1;
            s++;
        }
    }
    if (*s)
        return -1;
    /* network byte order in memory: a.b.c.d = bytes a,b,c,d (little-endian host) */
    *out_be = parts[0] | (parts[1] << 8) | (parts[2] << 16) | (parts[3] << 24);
    return 0;
}

static int setopt_int(int fd, int level, int name, int v)
{
    return (int)SC5(NR_setsockopt, fd, level, name, &v, sizeof v);
}

int os_tcp_listen(const char *ipv4, int port)
{
    struct z_sockaddr_in sa;
    long fd, r;
    memset(&sa, 0, sizeof sa);
    sa.family = Z_AF_INET;
    sa.port = z_htons((uint16_t)port);
    if (parse_ipv4(ipv4, &sa.addr) < 0)
        return -OS_EINVAL;
    fd = SC3(NR_socket, Z_AF_INET, Z_SOCK_STREAM | Z_O_NONBLOCK | Z_O_CLOEXEC, 0);
    if (is_err(fd))
        return (int)fd;
    setopt_int((int)fd, Z_SOL_SOCKET, Z_SO_REUSEADDR, 1);
    r = SC3(NR_bind, fd, &sa, sizeof sa);
    if (!is_err(r))
        r = SC2(NR_listen, fd, 8);
    if (is_err(r)) {
        SC1(NR_close, fd);
        return (int)r;
    }
    return (int)fd;
}

int os_accept(int lfd, char *peer, size_t peern)
{
    struct z_sockaddr_in sa;
    int sl = (int)sizeof sa;
    long fd;
    memset(&sa, 0, sizeof sa);
    fd = SC4(NR_accept4, lfd, &sa, &sl, Z_O_NONBLOCK | Z_O_CLOEXEC);
    if (is_err(fd))
        return (int)fd;
    if (peer && peern) {
        uint32_t a = sa.addr;
        os_snprintf(peer, peern, "%u.%u.%u.%u:%u", (unsigned)(a & 255u), (unsigned)((a >> 8) & 255u),
                    (unsigned)((a >> 16) & 255u), (unsigned)(a >> 24), (unsigned)z_htons(sa.port));
    }
    return (int)fd;
}

void os_tune_socket(int fd, int bufbytes)
{
    setopt_int(fd, Z_IPPROTO_TCP, Z_TCP_NODELAY, 1);
    setopt_int(fd, Z_SOL_SOCKET, Z_SO_KEEPALIVE, 1);
    setopt_int(fd, Z_IPPROTO_TCP, Z_TCP_KEEPIDLE, 10);
    setopt_int(fd, Z_IPPROTO_TCP, Z_TCP_KEEPINTVL, 5);
    setopt_int(fd, Z_IPPROTO_TCP, Z_TCP_KEEPCNT, 3);
    if (bufbytes > 0) {
        if (setopt_int(fd, Z_SOL_SOCKET, Z_SO_SNDBUFFORCE, bufbytes) < 0)
            setopt_int(fd, Z_SOL_SOCKET, Z_SO_SNDBUF, bufbytes);
        if (setopt_int(fd, Z_SOL_SOCKET, Z_SO_RCVBUFFORCE, bufbytes) < 0)
            setopt_int(fd, Z_SOL_SOCKET, Z_SO_RCVBUF, bufbytes);
    }
}

long os_recv(int fd, void *buf, size_t n)
{
    return SC6(NR_recvfrom, fd, buf, n, Z_MSG_DONTWAIT, 0, 0);
}

long os_send(int fd, const void *buf, size_t n)
{
    return SC6(NR_sendto, fd, buf, n, Z_MSG_DONTWAIT | Z_MSG_NOSIGNAL, 0, 0);
}

int os_close(int fd)
{
    return (int)SC1(NR_close, fd);
}

int os_poll(struct os_pollfd *p, int n, int64_t timeout_ns)
{
    struct z_timespec32 ts;
    if (timeout_ns < 0)
        return (int)SC5(NR_ppoll, p, n, 0, 0, 8);
    if (timeout_ns > 2000000000000LL)
        timeout_ns = 2000000000000LL;
    ts.tv_sec = (long)(timeout_ns / 1000000000);
    ts.tv_nsec = (long)(timeout_ns % 1000000000);
    return (int)SC5(NR_ppoll, p, n, &ts, 0, 8);
}

/* ---- files, mmap -------------------------------------------------------------------------- */
int os_open(const char *path, int writable, int osync)
{
    long flags = (writable ? Z_O_RDWR : Z_O_RDONLY) | Z_O_CLOEXEC | Z_O_LARGEFILE;
    if (osync)
        flags |= Z_O_SYNC;
    return (int)SC4(NR_openat, Z_AT_FDCWD, path, flags, 0);
}

void *os_mmap(int fd, uint64_t off, size_t len, int writable, int *err)
{
    long r;
    if (off & 4095u) {
        if (err)
            *err = -OS_EINVAL;
        return NULL;
    }
    r = SC6(NR_mmap2, 0, len, Z_PROT_READ | (writable ? Z_PROT_WRITE : 0), Z_MAP_SHARED, fd,
            (long)(off >> 12));
    if (is_err(r)) {
        if (err)
            *err = (int)r;
        return NULL;
    }
    if (err)
        *err = 0;
    return (void *)r;
}

void os_munmap(void *p, size_t len)
{
    if (p)
        SC2(NR_munmap, p, len);
}

long os_read(int fd, void *buf, size_t n)
{
    return SC3(NR_read, fd, buf, n);
}

long os_write(int fd, const void *buf, size_t n)
{
    return SC3(NR_write, fd, buf, n);
}

int os_read_text(const char *path, char *buf, size_t n)
{
    int fd;
    long r;
    if (n == 0)
        return -OS_EINVAL;
    fd = os_open(path, 0, 0);
    if (fd < 0)
        return fd;
    r = os_read(fd, buf, n - 1);
    os_close(fd);
    if (r < 0)
        return (int)r;
    buf[r] = 0;
    return (int)r;
}

/* ---- processes -------------------------------------------------------------------------- */
int os_fork(void)
{
    /* ARM clone(flags, newsp, parent_tid, tls, child_tid); newsp 0 = child runs on a copy of our stack */
    return (int)SC5(NR_clone, Z_SIGCHLD, 0, 0, 0, 0);
}

int os_pipe(int fds[2])
{
    return (int)SC2(NR_pipe2, fds, Z_O_CLOEXEC);
}

int os_wait(int pid, int *status, int nohang)
{
    return (int)SC4(NR_wait4, pid, status, nohang ? Z_WNOHANG : 0, 0);
}

int os_kill(int pid, int sig)
{
    return (int)SC2(NR_kill, pid, sig);
}

int os_status_signal(int status)
{
    int s = status & 0x7f;
    return (s != 0 && s != 0x7f) ? s : 0;
}

int os_status_exitcode(int status)
{
    return (status & 0x7f) == 0 ? (status >> 8) & 0xff : -1;
}

void os_no_coredump(void)
{
    unsigned long rl[2] = { 0, 0 };                    /* struct rlimit { cur, max } */
    SC2(NR_setrlimit, Z_RLIMIT_CORE, rl);
}

void os_ignore_sigpipe(void)
{
    /* kernel struct sigaction (ARM): handler, flags, restorer, 64-bit mask; SIG_IGN = 1 */
    unsigned long sa[5] = { 1, 0, 0, 0, 0 };
    SC4(NR_rt_sigaction, Z_SIGPIPE, sa, 0, 8);
}

void os_io_barrier(void)
{
    __asm__ volatile("dsb sy" ::: "memory");
}
