/*
 * tool_common.h -- option parsing and small helpers shared by the Pi tools.
 *
 * Common options (every tool):
 *   --fpga HOST[:PORT]   daemon address. Default: $FPGAGPU_HOST (same form) if set, else
 *                        10.77.0.2 (direct cable) then 10.20.0.2 (car LAN), port 7777
 *   --port N             daemon port
 *   --teensy SPEC        auto | DEVICE | tcp:HOST:PORT | none
 *   --timeout MS         reply timeout for ordinary requests (default 5000)
 *   -v                   verbose
 */
#ifndef PI_TOOL_COMMON_H
#define PI_TOOL_COMMON_H

#include <signal.h>
#include <stdint.h>
#include "libfpgagpu.h"

typedef struct {
    char        host[256];     /* "" = not given: $FPGAGPU_HOST or the defaults (libfpgagpu) */
    int         port;
    int         port_set;      /* port given by --port or --fpga HOST:PORT */
    const char *teensy;        /* option value (tool default applied by the caller) */
    int         timeout_ms;
    int         verbose;
} tool_opts;

void tool_opts_init(tool_opts *o, const char *teensy_default);
/* Handles argv[*i] if it is a common option (advancing *i past its value): 1 = handled,
 * 0 = not a common option, -1 = bad/missing value (message printed). */
int  tool_common_opt(tool_opts *o, int argc, char **argv, int *i);
const char *tool_common_help(void);

/* connect to the daemon; prints the reason and returns NULL on failure. On success o->host/o->port
 * become the address actually used (so further connections go straight there). */
gpu_conn *tool_connect(tool_opts *o, int observer, const char *prog);
/* the daemon address for messages: "HOST:PORT", or the candidates when none was given */
const char *tool_host_desc(const tool_opts *o);
/* open the Teensy per o->teensy; required = 1: failure is fatal (NULL + message);
 * returns NULL for "none" or (required = 0) when autodetection finds nothing */
teensy_conn *tool_teensy(const tool_opts *o, int required, const char *prog, int *failed);

extern volatile sig_atomic_t tool_stop;     /* set by SIGINT / SIGTERM */
void tool_catch_signals(void);

/* parse helpers: 0 = ok */
int tool_parse_int(const char *s, long lo, long hi, long *out);
int tool_parse_double(const char *s, double lo, double hi, double *out);

#endif
