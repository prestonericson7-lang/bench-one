/*
 * backend.h -- the PL as seen by fpgagpud: GP0 registers, the PS command FIFO, the reserved DDR
 * window. Two implementations with identical register semantics (SPEC 7, 9, 10, 13):
 *   backend_hw.c   real Zynq: /dev/mem (devcfg PCFG_DONE gate, reserved-window check, GP0 at
 *                  0x43C00000, DDR window)
 *   backend_sim.c  software PL model (fpgagpud --sim): FIFOs + collector + gpu_refrast_frame,
 *                  32 MB virtual DDR, immediate swap, simulated Teensy bus on TCP 7778.
 * The daemon logic in fpgagpud.c is shared and only talks to this interface.
 */
#ifndef FPGAGPUD_BACKEND_H
#define FPGAGPUD_BACKEND_H

#include <stddef.h>
#include <stdint.h>
#include "os.h"

typedef struct backend backend;

typedef struct {
    /* Is a PL with our ID/VERSION configured? 1 = yes (registers usable from now on),
     * 0 = no (reason in why). Never touches GP0 unless devcfg says the PL is configured.
     * Called at start-up and every few seconds while not ready. */
    int  (*probe)(backend *b, char *why, size_t whyn);
    /* Cheap check while ready (no GP0 access): 0 = PL no longer configured. */
    int  (*alive)(backend *b);
    uint32_t (*rd)(backend *b, uint32_t off);            /* GP0 register read  (only when ready) */
    void (*wr)(backend *b, uint32_t off, uint32_t v);    /* GP0 register write (only when ready) */
    /* Push n whole records (word 0 -> PS_FIFO_SOR, words 1..23 -> PS_FIFO_DATA). The caller has
     * checked PS_FIFO_FREE >= 24*n. Ends with an I/O barrier. */
    void (*push)(backend *b, const uint32_t *recs, int n);
    /* DDR window access by physical address; addr and len multiples of 8; 0 or -errno. */
    int  (*ddr_write)(backend *b, uint32_t phys, const void *src, size_t len);
    int  (*ddr_read)(backend *b, uint32_t phys, void *dst, size_t len);
    /* event-loop integration (sim: Teensy bus socket); may be NULL */
    int  (*pollfds)(backend *b, struct os_pollfd *p, int max);
    void (*service)(backend *b, const struct os_pollfd *p, int n);
    void (*destroy)(backend *b);
} backend_ops;

struct backend {
    const backend_ops *ops;
    const char        *name;
};

/* fake_devmem: NULL for /dev/mem. For tests, a regular file with the pages laid out as
 * 0x0000 devcfg (0xF8007000), 0x1000 SLCR (0xF8000000), 0x2000 GP0 page at 0x40000000,
 * 0x3000 GP0 registers (0x43C00000), 0x4000.. the 32 MB DDR window (0x1E000000). A page beyond
 * the end of the file faults (SIGBUS) when touched -- which is how tests prove it is not.
 * cmdline: file holding the kernel command line (NULL: /proc/cmdline, or none with fake_devmem);
 * "fpgagpu.pl_loaded=1" in it means U-Boot loaded pl.bit (see backend_hw.c).
 * iomem: file in /proc/iomem format (NULL: /proc/iomem, or no check with fake_devmem); the PL is
 * only used when no "System RAM" range overlaps the DDR window (it must be reserved no-map). */
backend *backend_hw_create(const char *fake_devmem, const char *cmdline, const char *iomem);

typedef struct {
    const char *bind_ip;      /* Teensy bus listener address */
    int         bus_port;     /* 7778 */
    int         nopl;         /* 1: behave like an unconfigured PL forever */
    int         pl_delay_ms;  /* >0: the PL "gets configured" this long after start */
} sim_opts;
backend *backend_sim_create(const sim_opts *o);

#endif
