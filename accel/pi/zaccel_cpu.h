/* zaccel_cpu -- the Orange Pi's own GEMV on every core: the honest "Pi alone" baseline.
 *
 * Same packed weights and same answer as zaccel_ref_gemv(), bit for bit.  Built with
 * -march=armv8.2-a+dotprod it runs NEON SDOT (vdotq_s32); anywhere else a portable C path.
 * A persistent pthread pool; rows are handed out in small chunks from an atomic counter, so the
 * A733's two fast A76 cores take more rows than its six A55 cores instead of waiting for them.
 * One call at a time per pool.
 */
#ifndef ZACCEL_CPU_H
#define ZACCEL_CPU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zaccel_cpu zaccel_cpu_t;

zaccel_cpu_t *zaccel_cpu_create(int nthreads);   /* <= 0: every online core; NULL on failure */
void          zaccel_cpu_destroy(zaccel_cpu_t *p);
int           zaccel_cpu_threads(const zaccel_cpu_t *p);
const char   *zaccel_cpu_kernel(void);            /* "neon-sdot" or "portable-c" */

/* packed: rows * zaccel_row_bytes(mode, cols).  A: int8 [nb][cols].  Y: int32 [rows][nb].
 * cols 1..4096, nb 1..8.  Returns 0, or ZACCEL_E_ARG. */
int zaccel_cpu_gemv(zaccel_cpu_t *p, uint32_t mode, uint32_t rows, uint32_t cols,
                    const uint8_t *packed, uint32_t nb, const int8_t *A, int32_t *Y);

#ifdef __cplusplus
}
#endif
#endif
