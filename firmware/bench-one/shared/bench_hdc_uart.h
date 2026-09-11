/* ===========================================================================================
 *  bench_hdc_uart.h -- the Teensy's link to its Luckfox
 * ===========================================================================================
 *
 *  WHY THIS EXISTS
 *  ----------------
 *  A Teensy holds 256 concepts locally and answers those in microseconds. Everything else it
 *  has to ask the cluster, and it has no IP stack -- deliberately, because a Teensy that is
 *  running TCP is a Teensy that is not being a real-time front end. Its Luckfox is its network
 *  stack: the Teensy sends a query over the wire, the Luckfox runs it against all eleven nodes
 *  and hands back the answer.
 *
 *  Local hit -> microseconds. Local miss -> one UART round trip. That is a cache hierarchy and
 *  it is the same shape as L1 -> RAM, for the same reason.
 *
 *
 *  THE CONSTRAINT THAT SHAPED THE FORMAT
 *  --------------------------------------
 *  A hypervector is 1024 bytes. The project's tested protocol caps a payload at
 *  IOP_MAX_PAYLOAD_LEN = 1015. It does not fit, and interop_protocol.h is byte-locked and not
 *  going to be changed to make it fit.
 *
 *  So a query FRAGMENTS: two frames of 512 bytes. That costs nothing real -- at 6 Mbaud the
 *  whole exchange is under 2 ms -- and it leaves the tested codec untouched.
 *
 *
 *  BANDWIDTH, SO NOBODY BUYS ETHERNET THEY DO NOT NEED
 *  ----------------------------------------------------
 *      921600 baud   1040 bytes out + 16 back   ~11.5 ms   ~87 queries/sec
 *      6 Mbaud       same                        ~1.8 ms   ~570 queries/sec
 *
 *  The Teensy only spends this on a MISS. 570 misses/second is far past what the design needs,
 *  which is why the Teensy Ethernet kit stays optional and the Luckfox magjack does not.
 *
 *
 *  CRC-16/MCRF4XX, matching the rest of the project
 *  -------------------------------------------------
 *  Poly 0x1021 reflected (0x8408), init 0xFFFF, no final xor. Check value over "123456789" is
 *  0x6F91 and hdu_selftest() asserts it. Same CRC as interop_protocol.h so there is exactly one
 *  checksum in this machine and no chance of two implementations quietly disagreeing.
 * ===========================================================================================
 */

#ifndef BENCH_HDC_UART_H
#define BENCH_HDC_UART_H

#include "bench_hdc.h"
#include "bench_hdc_shard.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HDU_START      0xA7u        /* deliberately NOT 0xAA: a stray HDC frame must never be
                                     * mistaken for a tested-protocol frame on a shared line  */
#define HDU_FRAG_BYTES 512u
#define HDU_FRAGS      (HD_BYTES / HDU_FRAG_BYTES)   /* 2 at HD_BITS=8192 */

/* Frame: START | TYPE | QID_L | QID_H | FRAG | LEN_L | LEN_H | DATA... | CRC_L | CRC_H
 * CRC covers TYPE through the last DATA byte. */
#define HDU_HDR_BYTES  7u
#define HDU_MAX_DATA   HDU_FRAG_BYTES
#define HDU_MAX_FRAME  (HDU_HDR_BYTES + HDU_MAX_DATA + 2u)

typedef enum {
    HDU_T_QUERY  = 0x01u,   /* Teensy -> Luckfox: recognise this (fragmented)                */
    HDU_T_STORE  = 0x02u,   /* Teensy -> Luckfox: remember this  (fragmented)                */
    HDU_T_RESULT = 0x03u,   /* Luckfox -> Teensy: 16-byte hd_partial_t                        */
    HDU_T_DEEP   = 0x04u,   /* Luckfox -> Teensy: id + full-D distance + margin               */
    HDU_T_PING   = 0x05u,
} hdu_type_t;

/* What comes back from a deep (full-cluster) answer. 12 bytes on the wire. */
typedef struct {
    int32_t  id;            /* global concept id, or -1                                      */
    uint32_t dist;          /* distance at HD_BITS * slices                                  */
    uint16_t margin_q8;     /* separation from runner-up, 1/256ths of a sigma                */
    uint8_t  slices;        /* how many slices contributed -- 0 means WITHHELD               */
    uint8_t  coverage;      /* 0..255 of the memory actually searched                        */
} hdu_deep_result_t;

/* -------------------------------------------------------------------------------------------
 * ENCODE
 * ----------------------------------------------------------------------------------------- */

/* Build fragment `frag` (0..HDU_FRAGS-1) of a hypervector message into `out`.
 * Returns bytes written, or 0 if `cap` is too small. Send all fragments in order. */
uint32_t hdu_encode_hv(uint8_t type, uint16_t qid, const hd_t v, uint8_t frag,
                       uint8_t *out, uint32_t cap);

uint32_t hdu_encode_result(uint16_t qid, const hd_partial_t *p, uint8_t *out, uint32_t cap);
uint32_t hdu_encode_deep(uint16_t qid, const hdu_deep_result_t *r, uint8_t *out, uint32_t cap);

/* -------------------------------------------------------------------------------------------
 * DECODE -- byte-at-a-time, so it drops into any serialEvent or read() loop
 * ----------------------------------------------------------------------------------------- */
typedef struct {
    uint8_t  buf[HDU_MAX_FRAME];
    uint16_t have;
    uint16_t want;
    uint8_t  state;

    /* reassembly */
    hd_t     hv;
    uint16_t hv_qid;
    uint8_t  hv_type;
    uint8_t  frags_in;      /* bitmask; complete when all HDU_FRAGS bits are set             */

    /* stats -- a live quality read on the link, free */
    uint32_t frames_ok;
    uint32_t crc_bad;
    uint32_t resyncs;
} hdu_rx_t;

void hdu_rx_init(hdu_rx_t *rx);

/* Feed one byte. Returns:
 *   0  nothing yet
 *   HDU_T_QUERY / HDU_T_STORE  -- a COMPLETE hypervector is in rx->hv, id in rx->hv_qid
 *   HDU_T_RESULT / HDU_T_DEEP / HDU_T_PING -- a short frame; payload accessors below
 * A bad CRC is counted and the parser resynchronises; it never blocks or returns garbage. */
uint8_t hdu_rx_feed(hdu_rx_t *rx, uint8_t byte);

/* Valid immediately after hdu_rx_feed returns the matching type. */
int hdu_get_result(const hdu_rx_t *rx, hd_partial_t *out);
int hdu_get_deep  (const hdu_rx_t *rx, hdu_deep_result_t *out);

/* CRC-16/MCRF4XX. Exposed so a test can prove both ends agree. */
uint16_t hdu_crc16(const uint8_t *d, uint32_t n);

/* Returns 1 if the CRC reproduces the project's check value 0x6F91 over "123456789".
 * Call it once at boot: two nodes with different CRCs present as a link that is up and
 * passes nothing, which is among the worst things to debug on a bench. */
int hdu_selftest(void);

#ifdef __cplusplus
}
#endif
#endif /* BENCH_HDC_UART_H */
