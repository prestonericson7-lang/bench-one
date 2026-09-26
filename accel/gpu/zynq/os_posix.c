/*
 * os_posix.c -- os.h on top of a normal libc (glibc). Used by the x86-64 simulator build
 * (fpgagpud_sim) and by the ARM build when an armhf libc is available (make arm ARM_LIBC=glibc).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "os.h"

void os_log(const char *fmt, ...)
{
    char buf[1024];
    int n;
    va_list ap;
    memcpy(buf, "fpgagpud: ", 10);
    va_start(ap, fmt);
    n = vsnprintf(buf + 10, sizeof buf - 11, fmt, ap);
    va_end(ap);
    if (n < 0)
        n = 0;
    n += 10;
    if (n > (int)sizeof buf - 1)
        n = (int)sizeof buf - 1;
    buf[n++] = '\n';
    if (write(2, buf, (size_t)n) < 0) {
        /* nothing sensible to do */
    }
}

int os_vsnprintf(char *buf, size_t n, const char *fmt, va_list ap)
{
    return vsnprintf(buf, n, fmt, ap);
}

int os_snprintf(char *buf, size_t n, const char *fmt, ...)
{
    int r;
    va_list ap;
    va_start(ap, fmt);
    r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

const char *os_strerror(int neg_errno)
{
    return strerror(neg_errno < 0 ? -neg_errno : neg_errno);
}

uint64_t os_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

void *os_malloc(size_t n) { return malloc(n ? n : 1); }
void *os_calloc(size_t n) { return calloc(1, n ? n : 1); }
void *os_realloc(void *p, size_t n) { return realloc(p, n ? n : 1); }
void  os_free(void *p) { free(p); }

int os_tcp_listen(const char *ipv4, int port)
{
    struct sockaddr_in sa;
    int fd, one = 1;

    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ipv4, &sa.sin_addr) != 1)
        return -EINVAL;
    fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -errno;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(fd, 8) < 0) {
        int e = errno;
        close(fd);
        return -e;
    }
    return fd;
}

int os_accept(int lfd, char *peer, size_t peern)
{
    struct sockaddr_in sa;
    socklen_t sl = sizeof sa;
    int fd = accept4(lfd, (struct sockaddr *)&sa, &sl, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0)
        return -errno;
    if (peer && peern) {
        unsigned a = ntohl(sa.sin_addr.s_addr);
        snprintf(peer, peern, "%u.%u.%u.%u:%u", a >> 24, (a >> 16) & 255u, (a >> 8) & 255u,
                 a & 255u, (unsigned)ntohs(sa.sin_port));
    }
    return fd;
}

void os_tune_socket(int fd, int bufbytes)
{
    int one = 1, idle = 10, intvl = 5, cnt = 3;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof intvl);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt);
    if (bufbytes > 0) {
        /* FORCE variants ignore rmem_max/wmem_max (needs CAP_NET_ADMIN; the daemon runs as root) */
        if (setsockopt(fd, SOL_SOCKET, SO_SNDBUFFORCE, &bufbytes, sizeof bufbytes) < 0)
            setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufbytes, sizeof bufbytes);
        if (setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &bufbytes, sizeof bufbytes) < 0)
            setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufbytes, sizeof bufbytes);
    }
}

long os_recv(int fd, void *buf, size_t n)
{
    ssize_t r = recv(fd, buf, n, MSG_DONTWAIT);
    return r < 0 ? -errno : (long)r;
}

long os_send(int fd, const void *buf, size_t n)
{
    ssize_t r = send(fd, buf, n, MSG_DONTWAIT | MSG_NOSIGNAL);
    return r < 0 ? -errno : (long)r;
}

int os_close(int fd) { return close(fd) < 0 ? -errno : 0; }

int os_poll(struct os_pollfd *p, int n, int64_t timeout_ns)
{
    struct timespec ts, *tp = NULL;
    int r;
    if (timeout_ns >= 0) {
        ts.tv_sec = (time_t)(timeout_ns / 1000000000);
        ts.tv_nsec = (long)(timeout_ns % 1000000000);
        tp = &ts;
    }
    r = ppoll((struct pollfd *)(void *)p, (nfds_t)n, tp, NULL);
    return r < 0 ? -errno : r;
}

int os_open(const char *path, int writable, int osync)
{
    int fd = open(path, (writable ? O_RDWR : O_RDONLY) | (osync ? O_SYNC : 0) | O_CLOEXEC);
    return fd < 0 ? -errno : fd;
}

void *os_mmap(int fd, uint64_t off, size_t len, int writable, int *err)
{
    void *p = mmap(NULL, len, PROT_READ | (writable ? PROT_WRITE : 0), MAP_SHARED, fd, (off_t)off);
    if (p == MAP_FAILED) {
        if (err)
            *err = -errno;
        return NULL;
    }
    if (err)
        *err = 0;
    return p;
}

void os_munmap(void *p, size_t len) { if (p) munmap(p, len); }

long os_read(int fd, void *buf, size_t n)
{
    ssize_t r = read(fd, buf, n);
    return r < 0 ? -errno : (long)r;
}

long os_write(int fd, const void *buf, size_t n)
{
    ssize_t r = write(fd, buf, n);
    return r < 0 ? -errno : (long)r;
}

int os_read_text(const char *path, char *buf, size_t n)
{
    int fd;
    long r;
    if (n == 0)
        return -EINVAL;
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -errno;
    r = read(fd, buf, n - 1);
    close(fd);
    if (r < 0)
        return -EIO;
    buf[r] = 0;
    return (int)r;
}

int os_fork(void)
{
    pid_t p = fork();
    return p < 0 ? -errno : (int)p;
}

int os_pipe(int fds[2]) { return pipe2(fds, O_CLOEXEC) < 0 ? -errno : 0; }

int os_wait(int pid, int *status, int nohang)
{
    pid_t r = waitpid(pid, status, nohang ? WNOHANG : 0);
    return r < 0 ? -errno : (int)r;
}

int os_kill(int pid, int sig) { return kill(pid, sig) < 0 ? -errno : 0; }

void os_exit(int code) { _exit(code); }

int os_status_signal(int status) { return WIFSIGNALED(status) ? WTERMSIG(status) : 0; }
int os_status_exitcode(int status) { return WIFEXITED(status) ? WEXITSTATUS(status) : -1; }

void os_no_coredump(void)
{
    struct rlimit rl;
    rl.rlim_cur = 0;
    rl.rlim_max = 0;
    setrlimit(RLIMIT_CORE, &rl);
}

void os_ignore_sigpipe(void)
{
    signal(SIGPIPE, SIG_IGN);
}

void os_io_barrier(void)
{
#if defined(__arm__)
    __asm__ volatile("dsb sy" ::: "memory");
#elif defined(__aarch64__)
    __asm__ volatile("dsb sy" ::: "memory");
#else
    __sync_synchronize();
#endif
}
