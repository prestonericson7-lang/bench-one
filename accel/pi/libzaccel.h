/* libzaccel -- the Orange Pi's client for the Zynq matrix engine (contract: accel/SPEC.md §4, §5)
 *
 *   zaccel_t *z = zaccel_connect(NULL, 0);          // $ZACCEL_HOST or 10.20.0.2, port 8093
 *   zaccel_pack_int4(w, rows, cols, packed);         // int8 weights -8..7 -> wire rows
 *   zaccel_tensor_t t;  zaccel_load(z, ZACCEL_MODE_INT4, rows, cols, packed, &t);
 *   zaccel_gemv(z, &t, nb, A, Y, &cycles, &engine); // A int8 [nb][cols], Y int32 [rows][nb]
 *   zaccel_ref_gemv(...)                             // the CPU reference every answer is checked against
 *
 * Packed weight rows follow SPEC §1 exactly: every row starts on a fresh 64-bit beat, so a row is
 * zaccel_row_bytes(mode, cols) bytes -- 8*ceil(cols/16) for int4, 8*ceil(cols/8) for int8 -- with
 * zero tail padding.  int4 bytes hold weight 2i in bits 3:0 and weight 2i+1 in bits 7:4 (signed).
 *
 * C11, no dependencies beyond the C library and BSD sockets.  A zaccel_t is one TCP connection and
 * carries one request at a time: do not share one between threads without a lock.
 */
#ifndef LIBZACCEL_H
#define LIBZACCEL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZACCEL_DEFAULT_PORT 8093
#define ZACCEL_DEFAULT_HOST "10.20.0.2"
#define ZACCEL_ALT_HOST     "10.77.0.2"   /* tried when no host is given and DEFAULT does not answer */
#define ZACCEL_MAX_COLS     4096
#define ZACCEL_MAX_BATCH    8

#define ZACCEL_MODE_INT4 0u   /* W int4 x A int8 */
#define ZACCEL_MODE_INT8 1u   /* W int8 x A int8 */

/* Return codes.  Positive values are the server's reply status (SPEC §4), negative are local. */
#define ZACCEL_OK           0
#define ZACCEL_ST_BADREQ    1   /* server: bad request            */
#define ZACCEL_ST_NOMEM     2   /* server: no memory              */
#define ZACCEL_ST_ENGINE    3   /* server: engine error           */
#define ZACCEL_ST_NOTENSOR  4   /* server: unknown tensor         */
#define ZACCEL_E_IO        -1   /* socket error / timeout; the connection is closed   */
#define ZACCEL_E_PROTO     -2   /* malformed reply; the connection is closed          */
#define ZACCEL_E_ARG       -3   /* bad argument; nothing was sent                     */
#define ZACCEL_E_NOMEM     -4   /* local allocation failed                            */
#define ZACCEL_E_CLOSED    -5   /* the connection was already closed by an earlier error */

typedef struct zaccel zaccel_t;

typedef struct {
    uint32_t version;       /* 1                          */
    uint32_t engine;        /* 0 = cpu fallback, 1 = pl   */
    uint32_t mem_total_mb;
    uint32_t mem_free_mb;
    uint32_t max_cols;      /* 4096                       */
    uint32_t max_batch;     /* 8                          */
    uint32_t selftest;      /* 0 = pass, else fail code   */
} zaccel_info_t;

typedef struct {
    uint32_t id;            /* server tensor id           */
    uint32_t mode, rows, cols;
} zaccel_tensor_t;

/* host NULL or "": $ZACCEL_HOST, else ZACCEL_DEFAULT_HOST.  port <= 0: ZACCEL_DEFAULT_PORT.
 * Returns NULL on failure with errno set.  Connect gives up after 3 s; every later send/receive
 * after 60 s ($ZACCEL_TIMEOUT_MS overrides both). */
zaccel_t   *zaccel_connect(const char *host, int port);
const char *zaccel_default_host(void);          /* $ZACCEL_HOST or ZACCEL_DEFAULT_HOST       */
void        zaccel_close(zaccel_t *z);          /* NULL is fine                              */

int zaccel_info(zaccel_t *z, zaccel_info_t *out);
/* Sends len bytes, checks the echo is identical (ZACCEL_E_PROTO if not). */
int zaccel_ping(zaccel_t *z, const void *data, uint32_t len);
/* packed: rows * zaccel_row_bytes(mode, cols) bytes.  Fills *t on success. */
int zaccel_load(zaccel_t *z, uint32_t mode, uint32_t rows, uint32_t cols,
                const uint8_t *packed, zaccel_tensor_t *t);
int zaccel_free(zaccel_t *z, const zaccel_tensor_t *t);
/* A: int8 [nb][t->cols], plain.  Y: int32 [t->rows][nb].  cycles / engine_used may be NULL. */
int zaccel_gemv(zaccel_t *z, const zaccel_tensor_t *t, uint32_t nb, const int8_t *A,
                int32_t *Y, uint32_t *cycles, uint32_t *engine_used);

const char *zaccel_strerror(int rc);

/* ---- packing and the reference ------------------------------------------------------------ */
size_t zaccel_row_bytes(uint32_t mode, uint32_t cols);   /* 0 for a bad mode */
/* w: int8 [rows][cols] with every value in -8..7.  out: rows * zaccel_row_bytes(0, cols).
 * Returns ZACCEL_OK, or ZACCEL_E_ARG if any value was outside -8..7 (its low nibble was packed). */
int  zaccel_pack_int4(const int8_t *w, uint32_t rows, uint32_t cols, uint8_t *out);
/* w: int8 [rows][cols].  out: rows * zaccel_row_bytes(1, cols); tail bytes zeroed. */
void zaccel_pack_int8(const int8_t *w, uint32_t rows, uint32_t cols, uint8_t *out);
/* Y[r][v] = sum_k W[r][k] * A[v][k], exact.  Reads the packed rows; weights past cols ignored. */
void zaccel_ref_gemv(uint32_t mode, uint32_t rows, uint32_t cols, const uint8_t *packed,
                     uint32_t nb, const int8_t *A, int32_t *Y);

#ifdef __cplusplus
}
#endif
#endif
