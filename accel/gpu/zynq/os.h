/*
 * os.h -- the only OS interface fpgagpud uses (sockets, poll, time, memory, /dev/mem, fork).
 *
 * Two implementations:
 *   os_posix.c   glibc / any POSIX libc (x86-64 simulator build, or ARM if an armhf libc exists)
 *   zrt/zrt.c    freestanding ARMv7-A EABI Linux runtime: raw syscalls, no libc at all. Used for
 *                the static Zynq binary because this toolchain has no armhf libc to link against.
 *
 * Conventions: functions that can fail return -errno (negative) like raw Linux syscalls.
 */
#ifndef FPGAGPUD_OS_H
#define FPGAGPUD_OS_H

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

/* identical to struct pollfd / POLL* on Linux (all architectures) */
struct os_pollfd {
    int   fd;
    short events;
    short revents;
};
#define OS_POLLIN    0x001
#define OS_POLLOUT   0x004
#define OS_POLLERR   0x008
#define OS_POLLHUP   0x010
#define OS_POLLNVAL  0x020

#define OS_EPERM     1
#define OS_ENOENT    2
#define OS_EINTR     4
#define OS_EIO       5
#define OS_EAGAIN    11
#define OS_ENOMEM    12
#define OS_EACCES    13
#define OS_EFAULT    14
#define OS_EINVAL    22

#define OS_SIGKILL   9

/* ---- logging / formatting (stderr -> journald) ------------------------------------------ */
void os_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int  os_snprintf(char *buf, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int  os_vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
const char *os_strerror(int neg_errno);          /* text for a -errno value */

/* ---- time ---------------------------------------------------------------------------------- */
uint64_t os_now_ns(void);                          /* CLOCK_MONOTONIC */

/* ---- memory -------------------------------------------------------------------------------- */
void *os_malloc(size_t n);                         /* NULL on failure; >= 8-byte aligned */
void *os_calloc(size_t n);                         /* zero-filled */
void *os_realloc(void *p, size_t n);
void  os_free(void *p);

/* ---- TCP ----------------------------------------------------------------------------------- */
int  os_tcp_listen(const char *ipv4, int port);    /* non-blocking listening socket or -errno */
int  os_accept(int lfd, char *peer, size_t peern); /* non-blocking client fd or -errno */
void os_tune_socket(int fd, int bufbytes);         /* TCP_NODELAY, big buffers, keepalive */
long os_recv(int fd, void *buf, size_t n);         /* >0 bytes, 0 = EOF, -errno (-OS_EAGAIN) */
long os_send(int fd, const void *buf, size_t n);   /* >=0 bytes, -errno; never raises SIGPIPE */
int  os_close(int fd);
int  os_poll(struct os_pollfd *p, int n, int64_t timeout_ns);   /* timeout < 0: infinite */

/* ---- files / physical memory -------------------------------------------------------------- */
int   os_open(const char *path, int writable, int osync);       /* fd or -errno */
void *os_mmap(int fd, uint64_t off, size_t len, int writable, int *err);  /* MAP_SHARED */
void  os_munmap(void *p, size_t len);
long  os_read(int fd, void *buf, size_t n);
long  os_write(int fd, const void *buf, size_t n);
int   os_read_text(const char *path, char *buf, size_t n);     /* NUL-terminated; bytes or -errno */

/* ---- processes (used only for the fault-isolated PL probe) -------------------------------- */
int  os_fork(void);                                /* 0 in the child, pid in the parent, -errno */
int  os_pipe(int fds[2]);
int  os_wait(int pid, int *status, int nohang);    /* pid, 0 = still running (nohang), -errno */
int  os_kill(int pid, int sig);
void os_exit(int code) __attribute__((noreturn)); /* _exit(): no cleanup */
int  os_status_signal(int status);                 /* terminating signal, 0 if exited normally */
int  os_status_exitcode(int status);
void os_no_coredump(void);                         /* RLIMIT_CORE = 0 (probe child) */
void os_ignore_sigpipe(void);                      /* SIGPIPE -> SIG_IGN (stderr to journald) */

/* ---- device I/O ordering ------------------------------------------------------------------- */
void os_io_barrier(void);                          /* ARM: dsb sy (all earlier accesses completed) */

#endif
