/* ===========================================================================================
 *  bench_hdc_uart.c -- see bench_hdc_uart.h
 * ===========================================================================================
 */

#include "bench_hdc_uart.h"

/* CRC-16/MCRF4XX: poly 0x1021 reflected = 0x8408, init 0xFFFF, no final xor.
 * Bitwise rather than table-driven on purpose -- 256 entries of table would cost 512 bytes of
 * DTCM on a Teensy to save time on a link that is already 100x slower than the CPU. */
uint16_t hdu_crc16(const uint8_t *d, uint32_t n)
{
    uint16_t crc = 0xFFFFu;
    for (uint32_t i = 0; i < n; i++) {
        crc ^= d[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 1u) ? (uint16_t)((crc >> 1) ^ 0x8408u) : (uint16_t)(crc >> 1);
    }
    return crc;
}

int hdu_selftest(void)
{
    static const uint8_t check[9] = { '1','2','3','4','5','6','7','8','9' };
    return (hdu_crc16(check, 9) == 0x6F91u) ? 1 : 0;
}

/* -------------------------------------------------------------------------------------------
 * ENCODE
 * ----------------------------------------------------------------------------------------- */
static uint32_t frame(uint8_t type, uint16_t qid, uint8_t frag,
                      const uint8_t *data, uint16_t len, uint8_t *out, uint32_t cap)
{
    const uint32_t need = HDU_HDR_BYTES + (uint32_t)len + 2u;
    if (cap < need || len > HDU_MAX_DATA) return 0;

    out[0] = HDU_START;
    out[1] = type;
    out[2] = (uint8_t)qid;
    out[3] = (uint8_t)(qid >> 8);
    out[4] = frag;
    out[5] = (uint8_t)len;
    out[6] = (uint8_t)(len >> 8);
    for (uint16_t i = 0; i < len; i++) out[HDU_HDR_BYTES + i] = data[i];

    /* CRC spans TYPE..last data byte. START is excluded because it is a marker, not content --
     * same reasoning as the tested protocol, and it keeps resync cheap. */
    const uint16_t c = hdu_crc16(out + 1, (uint32_t)(HDU_HDR_BYTES - 1u) + len);
    out[HDU_HDR_BYTES + len]      = (uint8_t)c;
    out[HDU_HDR_BYTES + len + 1u] = (uint8_t)(c >> 8);
    return need;
}

uint32_t hdu_encode_hv(uint8_t type, uint16_t qid, const hd_t v, uint8_t frag,
                       uint8_t *out, uint32_t cap)
{
    if (frag >= HDU_FRAGS) return 0;
    const uint8_t *src = (const uint8_t *)v + (uint32_t)frag * HDU_FRAG_BYTES;
    return frame(type, qid, frag, src, (uint16_t)HDU_FRAG_BYTES, out, cap);
}

uint32_t hdu_encode_result(uint16_t qid, const hd_partial_t *p, uint8_t *out, uint32_t cap)
{
    uint8_t w[HD_PARTIAL_WIRE_BYTES];
    hd_partial_pack(p, w);
    return frame(HDU_T_RESULT, qid, 0, w, (uint16_t)sizeof(w), out, cap);
}

uint32_t hdu_encode_deep(uint16_t qid, const hdu_deep_result_t *r, uint8_t *out, uint32_t cap)
{
    uint8_t w[12];
    const uint32_t id = (uint32_t)r->id;
    w[0]=(uint8_t)id;        w[1]=(uint8_t)(id>>8);  w[2]=(uint8_t)(id>>16); w[3]=(uint8_t)(id>>24);
    w[4]=(uint8_t)r->dist;   w[5]=(uint8_t)(r->dist>>8);
    w[6]=(uint8_t)(r->dist>>16); w[7]=(uint8_t)(r->dist>>24);
    w[8]=(uint8_t)r->margin_q8; w[9]=(uint8_t)(r->margin_q8>>8);
    w[10]=r->slices;         w[11]=r->coverage;
    return frame(HDU_T_DEEP, qid, 0, w, 12u, out, cap);
}

/* -------------------------------------------------------------------------------------------
 * DECODE
 * ----------------------------------------------------------------------------------------- */
enum { S_START = 0, S_HDR, S_DATA };

void hdu_rx_init(hdu_rx_t *rx)
{
    rx->have = 0; rx->want = 0; rx->state = S_START;
    rx->hv_qid = 0; rx->hv_type = 0; rx->frags_in = 0;
    rx->frames_ok = 0; rx->crc_bad = 0; rx->resyncs = 0;
}

static uint8_t finish(hdu_rx_t *rx)
{
    const uint16_t len = (uint16_t)(rx->buf[5] | ((uint16_t)rx->buf[6] << 8));
    const uint16_t got = (uint16_t)(rx->buf[HDU_HDR_BYTES + len]
                        | ((uint16_t)rx->buf[HDU_HDR_BYTES + len + 1u] << 8));
    const uint16_t want = hdu_crc16(rx->buf + 1, (uint32_t)(HDU_HDR_BYTES - 1u) + len);

    rx->state = S_START;
    rx->have  = 0;

    if (got != want) { rx->crc_bad++; return 0; }
    rx->frames_ok++;

    const uint8_t type = rx->buf[1];
    const uint16_t qid = (uint16_t)(rx->buf[2] | ((uint16_t)rx->buf[3] << 8));
    const uint8_t frag = rx->buf[4];

    if (type == HDU_T_QUERY || type == HDU_T_STORE) {
        if (frag >= HDU_FRAGS || len != HDU_FRAG_BYTES) return 0;

        /* A fragment from a different query starts a new reassembly. Without this a dropped
         * fragment would leave half of an old vector in the buffer and the next query would be
         * answered against a chimera of two -- which looks like the model being wrong, not the
         * link being lossy. */
        if (rx->frags_in == 0u || qid != rx->hv_qid || type != rx->hv_type) {
            rx->hv_qid  = qid;
            rx->hv_type = type;
            rx->frags_in = 0;
        }
        uint8_t *dst = (uint8_t *)rx->hv + (uint32_t)frag * HDU_FRAG_BYTES;
        for (uint16_t i = 0; i < HDU_FRAG_BYTES; i++) dst[i] = rx->buf[HDU_HDR_BYTES + i];
        rx->frags_in |= (uint8_t)(1u << frag);

        if (rx->frags_in == (uint8_t)((1u << HDU_FRAGS) - 1u)) {
            rx->frags_in = 0;
            return type;
        }
        return 0;
    }
    return type;
}

uint8_t hdu_rx_feed(hdu_rx_t *rx, uint8_t byte)
{
    switch (rx->state) {
    case S_START:
        if (byte != HDU_START) { rx->resyncs++; return 0; }
        rx->buf[0] = byte;
        rx->have = 1;
        rx->state = S_HDR;
        return 0;

    case S_HDR:
        rx->buf[rx->have++] = byte;
        if (rx->have < HDU_HDR_BYTES) return 0;
        {
            const uint16_t len = (uint16_t)(rx->buf[5] | ((uint16_t)rx->buf[6] << 8));
            /* Length is checked BEFORE any data is accepted. A corrupted length is the one
             * field that can make a parser read past its own buffer, so it is rejected here
             * rather than trusted and bounded later. */
            if (len > HDU_MAX_DATA) { rx->state = S_START; rx->have = 0; rx->resyncs++; return 0; }
            rx->want = (uint16_t)(HDU_HDR_BYTES + len + 2u);
            rx->state = S_DATA;
        }
        return 0;

    case S_DATA:
        rx->buf[rx->have++] = byte;
        if (rx->have < rx->want) return 0;
        return finish(rx);

    default:
        rx->state = S_START;
        rx->have = 0;
        return 0;
    }
}

int hdu_get_result(const hdu_rx_t *rx, hd_partial_t *out)
{
    if (rx->buf[1] != HDU_T_RESULT) return 0;
    return hd_partial_unpack(out, rx->buf + HDU_HDR_BYTES);
}

int hdu_get_deep(const hdu_rx_t *rx, hdu_deep_result_t *out)
{
    if (rx->buf[1] != HDU_T_DEEP) return 0;
    const uint8_t *w = rx->buf + HDU_HDR_BYTES;
    out->id        = (int32_t)((uint32_t)w[0] | ((uint32_t)w[1] << 8)
                             | ((uint32_t)w[2] << 16) | ((uint32_t)w[3] << 24));
    out->dist      = (uint32_t)w[4] | ((uint32_t)w[5] << 8)
                   | ((uint32_t)w[6] << 16) | ((uint32_t)w[7] << 24);
    out->margin_q8 = (uint16_t)(w[8] | ((uint16_t)w[9] << 8));
    out->slices    = w[10];
    out->coverage  = w[11];
    return 1;
}
