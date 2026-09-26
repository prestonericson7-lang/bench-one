/* tool_common.c -- see tool_common.h. */
#include "tool_common.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

volatile sig_atomic_t tool_stop;

static void on_signal(int sig)
{
    (void)sig;
    tool_stop = 1;
}

void tool_catch_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);
}

int tool_parse_int(const char *s, long lo, long hi, long *out)
{
    char *end;
    long v;
    if (!s || !*s)
        return -1;
    errno = 0;
    v = strtol(s, &end, 0);
    if (errno || *end || v < lo || v > hi)
        return -1;
    *out = v;
    return 0;
}

int tool_parse_double(const char *s, double lo, double hi, double *out)
{
    char *end;
    double v;
    if (!s || !*s)
        return -1;
    v = strtod(s, &end);
    if (*end || !(v >= lo && v <= hi))
        return -1;
    *out = v;
    return 0;
}

void tool_opts_init(tool_opts *o, const char *teensy_default)
{
    memset(o, 0, sizeof *o);
    o->host[0] = 0;                 /* libfpgagpu: $FPGAGPU_HOST, else 10.77.0.2 then 10.20.0.2 */
    o->port = GPU_NET_PORT;
    o->teensy = teensy_default;
    o->timeout_ms = FGPU_DEFAULT_TIMEOUT;
}

const char *tool_common_help(void)
{
    return "  --fpga HOST[:PORT]  daemon address (default: $" FGPU_HOST_ENV " if set, else " FGPU_DEFAULT_HOST
           " then " FGPU_DEFAULT_HOST2 ", port 7777)\n"
           "  --port N            daemon TCP port\n"
           "  --teensy SPEC       auto | /dev/ttyACMn | /dev/serial/by-id/... | tcp:HOST:PORT | none\n"
           "  --timeout MS        reply timeout (default 5000)\n"
           "  -v                  verbose\n";
}

int tool_common_opt(tool_opts *o, int argc, char **argv, int *i)
{
    const char *a = argv[*i], *v = *i + 1 < argc ? argv[*i + 1] : NULL;
    long n;
    if (!strcmp(a, "-v") || !strcmp(a, "--verbose")) {
        o->verbose++;
        return 1;
    }
    if (strcmp(a, "--fpga") && strcmp(a, "--port") && strcmp(a, "--teensy") && strcmp(a, "--timeout"))
        return 0;
    if (!v) {
        fprintf(stderr, "%s needs a value\n", a);
        return -1;
    }
    (*i)++;
    if (!strcmp(a, "--fpga")) {
        const char *colon = strrchr(v, ':');
        if (colon && strchr(v, ':') == colon) {         /* HOST:PORT (IPv4 / name only) */
            if (tool_parse_int(colon + 1, 1, 65535, &n) < 0 || colon == v ||
                (size_t)(colon - v) >= sizeof o->host) {
                fprintf(stderr, "bad --fpga value '%s' (HOST or HOST:PORT)\n", v);
                return -1;
            }
            memcpy(o->host, v, (size_t)(colon - v));
            o->host[colon - v] = 0;
            o->port = (int)n;
            o->port_set = 1;
        } else {
            if (strlen(v) >= sizeof o->host) {
                fprintf(stderr, "--fpga value too long\n");
                return -1;
            }
            snprintf(o->host, sizeof o->host, "%s", v);
        }
    } else if (!strcmp(a, "--port")) {
        if (tool_parse_int(v, 1, 65535, &n) < 0) {
            fprintf(stderr, "bad --port '%s'\n", v);
            return -1;
        }
        o->port = (int)n;
        o->port_set = 1;
    } else if (!strcmp(a, "--teensy")) {
        o->teensy = v;
    } else {
        if (tool_parse_int(v, 100, 600000, &n) < 0) {
            fprintf(stderr, "bad --timeout '%s' (100..600000 ms)\n", v);
            return -1;
        }
        o->timeout_ms = (int)n;
    }
    return 1;
}

const char *tool_host_desc(const tool_opts *o)
{
    static char buf[320];
    const char *env = getenv(FGPU_HOST_ENV);
    if (o->host[0])
        snprintf(buf, sizeof buf, "%s:%d", o->host, o->port);
    else if (env && *env)
        snprintf(buf, sizeof buf, "%.256s (" FGPU_HOST_ENV ")", env);
    else
        snprintf(buf, sizeof buf, FGPU_DEFAULT_HOST " or " FGPU_DEFAULT_HOST2 " (port %d)", o->port);
    return buf;
}

gpu_conn *tool_connect(tool_opts *o, int observer, const char *prog)
{
    char err[600];
    struct sockaddr_in sa;
    socklen_t sl = sizeof sa;
    /* no --fpga: libfpgagpu picks $FPGAGPU_HOST or a default; its port applies unless --port was given */
    gpu_conn *g = gpu_connect(o->host, o->host[0] || o->port_set ? o->port : 0, observer, err, sizeof err);
    if (!g) {
        fprintf(stderr, "%s: cannot use the FPGA-GPU daemon at %s: %s\n", prog, tool_host_desc(o), err);
        return NULL;
    }
    /* remember the address used: later connections (observer, viewer) go straight there */
    if (!o->host[0] && getpeername(gpu_fd(g), (struct sockaddr *)&sa, &sl) == 0 && sa.sin_family == AF_INET &&
        inet_ntop(AF_INET, &sa.sin_addr, o->host, sizeof o->host)) {
        o->port = ntohs(sa.sin_port);
        o->port_set = 1;
        if (o->verbose)
            fprintf(stderr, "%s: daemon at %s:%d\n", prog, o->host, o->port);
    }
    gpu_set_timeout(g, o->timeout_ms);
    if (gpu_hello_info(g)->status != GPU_ERR_OK)
        fprintf(stderr, "%s: warning: daemon HELLO status %d (%s)\n", prog, gpu_hello_info(g)->status,
                fgpu_strerror(gpu_hello_info(g)->status));
    return g;
}

teensy_conn *tool_teensy(const tool_opts *o, int required, const char *prog, int *failed)
{
    teensy_conn *t = NULL;
    char err[400];
    int autodetect = !o->teensy || !*o->teensy || !strcmp(o->teensy, "auto");
    int r = teensy_open(o->teensy, &t, err, sizeof err);
    if (failed)
        *failed = 0;
    if (r < 0) {
        if (autodetect && !required) {
            fprintf(stderr, "%s: no Teensy used (%s)\n", prog, err);
            return NULL;
        }
        fprintf(stderr, "%s: Teensy '%s': %s\n", prog, o->teensy ? o->teensy : "auto", err);
        if (failed)
            *failed = 1;
        return NULL;
    }
    if (t) {
        teensy_set_timeout(t, o->timeout_ms);
        if (err[0])
            fprintf(stderr, "%s: %s\n", prog, err);
        if (o->verbose)
            fprintf(stderr, "%s: Teensy at %s (fw %u, fpga_ready %u, bus_enabled %u)\n", prog, teensy_device(t),
                    teensy_hello_info(t)->fw_version, teensy_hello_info(t)->fpga_ready,
                    teensy_hello_info(t)->bus_enabled);
    }
    return t;
}
