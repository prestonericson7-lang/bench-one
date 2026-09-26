/* img.c -- see img.h. */
#include "img.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void rgb565_to_rgb888(uint16_t p, uint8_t out[3])
{
    unsigned r5 = (p >> 11) & 31u, g6 = (p >> 5) & 63u, b5 = p & 31u;
    out[0] = (uint8_t)((r5 << 3) | (r5 >> 2));
    out[1] = (uint8_t)((g6 << 2) | (g6 >> 4));
    out[2] = (uint8_t)((b5 << 3) | (b5 >> 2));
}

static uint8_t *to_rgb(const uint16_t *px, int w, int h)
{
    size_t n = (size_t)w * (size_t)h, i, bytes = n * 3u;
    uint8_t *rgb = malloc(bytes ? bytes : 1u);
    if (!rgb)
        return NULL;
    for (i = 0; i < n; i++)
        rgb565_to_rgb888(px[i], rgb + i * 3u);
    return rgb;
}

static int write_file(const char *path, const void *a, size_t na, const void *b, size_t nb)
{
    FILE *f = fopen(path, "wb");
    int ok;
    if (!f)
        return -1;
    ok = fwrite(a, 1, na, f) == na && (nb == 0 || fwrite(b, 1, nb, f) == nb);
    if (fclose(f) != 0)
        ok = 0;
    return ok ? 0 : -1;
}

int img_write_ppm(const char *path, const uint16_t *px, int w, int h)
{
    char hdr[64];
    int n, r;
    uint8_t *rgb = to_rgb(px, w, h);
    if (!rgb)
        return -1;
    n = snprintf(hdr, sizeof hdr, "P6\n%d %d\n255\n", w, h);
    r = write_file(path, hdr, (size_t)n, rgb, (size_t)w * (size_t)h * 3u);
    free(rgb);
    return r;
}

/* ---- PNG ------------------------------------------------------------------------------------ */
typedef struct {
    uint8_t *p;
    size_t   n, cap;
    uint64_t bits;
    int      nbits;
    int      oom;
} obuf;

static void ob_byte(obuf *o, uint8_t b)
{
    if (o->oom)
        return;
    if (o->n == o->cap) {
        size_t cap = o->cap ? o->cap * 2u : 65536u;
        uint8_t *q = realloc(o->p, cap);
        if (!q) {
            o->oom = 1;
            return;
        }
        o->p = q;
        o->cap = cap;
    }
    o->p[o->n++] = b;
}

static void ob_bytes(obuf *o, const void *src, size_t n)
{
    const uint8_t *s = src;
    size_t i;
    for (i = 0; i < n; i++)
        ob_byte(o, s[i]);
}

static void ob_be32(obuf *o, uint32_t v)
{
    ob_byte(o, (uint8_t)(v >> 24));
    ob_byte(o, (uint8_t)(v >> 16));
    ob_byte(o, (uint8_t)(v >> 8));
    ob_byte(o, (uint8_t)v);
}

/* deflate bit writer: LSB first */
static void put_bits(obuf *o, uint32_t v, int n)
{
    o->bits |= (uint64_t)v << o->nbits;
    o->nbits += n;
    while (o->nbits >= 8) {
        ob_byte(o, (uint8_t)o->bits);
        o->bits >>= 8;
        o->nbits -= 8;
    }
}

/* Huffman codes are sent most-significant bit first */
static void put_code(obuf *o, uint32_t code, int n)
{
    uint32_t r = 0;
    int i;
    for (i = 0; i < n; i++)
        r |= ((code >> i) & 1u) << (n - 1 - i);
    put_bits(o, r, n);
}

static void put_lit(obuf *o, unsigned sym)
{
    if (sym < 144)
        put_code(o, 0x30u + sym, 8);
    else if (sym < 256)
        put_code(o, 0x190u + (sym - 144u), 9);
    else if (sym < 280)
        put_code(o, sym - 256u, 7);
    else
        put_code(o, 0xC0u + (sym - 280u), 8);
}

static const uint16_t len_base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
                                      67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
                                      4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t dist_base[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513,
                                       769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
                                       9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static void put_match(obuf *o, unsigned len, unsigned dist)
{
    int i = 28, d = 29;
    while (len_base[i] > len)
        i--;
    put_lit(o, 257u + (unsigned)i);
    if (len_extra[i])
        put_bits(o, len - len_base[i], len_extra[i]);
    while (dist_base[d] > dist)
        d--;
    put_code(o, (uint32_t)d, 5);
    if (dist_extra[d])
        put_bits(o, dist - dist_base[d], dist_extra[d]);
}

#define HBITS   15
#define WSIZE   32768u
#define MAXCHAIN 24

/* one fixed-Huffman block with greedy LZ77 (hash chains) */
static int deflate_fixed(obuf *o, const uint8_t *d, size_t n)
{
    int32_t *head = malloc(sizeof(int32_t) << HBITS);
    int32_t *prev = malloc(sizeof(int32_t) * WSIZE);
    size_t i = 0;
    if (!head || !prev) {
        free(head);
        free(prev);
        return -1;
    }
    memset(head, 0xFF, sizeof(int32_t) << HBITS);
    put_bits(o, 1, 1);                  /* BFINAL */
    put_bits(o, 1, 2);                  /* BTYPE = 01 fixed Huffman */
    while (i < n) {
        unsigned best = 0, bestd = 0;
        if (i + 3 <= n) {
            uint32_t hsh = (((uint32_t)d[i] << 16) | ((uint32_t)d[i + 1] << 8) | d[i + 2]) * 2654435761u >> (32 - HBITS);
            int32_t cand = head[hsh];
            int chain = MAXCHAIN;
            size_t maxlen = n - i < 258 ? n - i : 258;
            while (cand >= 0 && chain-- > 0 && i - (size_t)cand <= WSIZE) {
                const uint8_t *a = d + cand, *b = d + i;
                unsigned l = 0;
                while (l < maxlen && a[l] == b[l])
                    l++;
                if (l > best) {
                    best = l;
                    bestd = (unsigned)(i - (size_t)cand);
                    if (l == maxlen)
                        break;
                }
                cand = prev[(size_t)cand % WSIZE];
            }
            prev[i % WSIZE] = head[hsh];
            head[hsh] = (int32_t)i;
        }
        if (best >= 3) {
            size_t k;
            put_match(o, best, bestd);
            for (k = 1; k < best; k++) {        /* insert the skipped positions */
                size_t p = i + k;
                if (p + 3 <= n) {
                    uint32_t hsh = (((uint32_t)d[p] << 16) | ((uint32_t)d[p + 1] << 8) | d[p + 2]) * 2654435761u >>
                                   (32 - HBITS);
                    prev[p % WSIZE] = head[hsh];
                    head[hsh] = (int32_t)p;
                }
            }
            i += best;
        } else {
            put_lit(o, d[i]);
            i++;
        }
    }
    put_lit(o, 256);                    /* end of block */
    if (o->nbits > 0)
        put_bits(o, 0, 8 - o->nbits);   /* flush to a byte boundary */
    free(head);
    free(prev);
    return 0;
}

static uint32_t crc_table[256];

static void crc_init(void)
{
    uint32_t n, c;
    int k;
    if (crc_table[1])
        return;
    for (n = 0; n < 256; n++) {
        c = n;
        for (k = 0; k < 8; k++)
            c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}

static uint32_t crc32_buf(uint32_t crc, const uint8_t *p, size_t n)
{
    size_t i;
    crc = ~crc;
    for (i = 0; i < n; i++)
        crc = crc_table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}

static uint32_t adler32(const uint8_t *p, size_t n)
{
    uint32_t a = 1, b = 0;
    size_t i;
    for (i = 0; i < n; i++) {
        a = (a + p[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

static void chunk(obuf *out, const char type[4], const uint8_t *data, size_t n)
{
    uint32_t crc;
    ob_be32(out, (uint32_t)n);
    ob_bytes(out, type, 4);
    ob_bytes(out, data, n);
    crc = crc32_buf(0, (const uint8_t *)type, 4);
    crc = crc32_buf(crc, data, n);
    ob_be32(out, crc);
}

static int paeth(int a, int b, int c)
{
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    if (pa <= pb && pa <= pc)
        return a;
    return pb <= pc ? b : c;
}

int png_encode_rgb(const uint8_t *rgb, int w, int h, uint8_t **out, size_t *outlen)
{
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    size_t stride = (size_t)w * 3u, rawn = ((size_t)stride + 1u) * (size_t)h;
    uint8_t *raw, ihdr[13], *cand;
    obuf z, o;
    int y, f;

    *out = NULL;
    *outlen = 0;
    if (w <= 0 || h <= 0)
        return -1;
    crc_init();
    raw = malloc(rawn);
    cand = malloc(stride * 5u);
    if (!raw || !cand) {
        free(raw);
        free(cand);
        return -1;
    }
    /* per row: the filter with the smallest sum of |signed bytes| */
    for (y = 0; y < h; y++) {
        const uint8_t *cur = rgb + (size_t)y * stride, *up = y ? cur - stride : NULL;
        long best_sum = -1;
        int best = 0;
        size_t x;
        for (f = 0; f < 5; f++) {
            uint8_t *c = cand + (size_t)f * stride;
            long sum = 0;
            for (x = 0; x < stride; x++) {
                int a = x >= 3 ? cur[x - 3] : 0, b = up ? up[x] : 0, cc = (x >= 3 && up) ? up[x - 3] : 0;
                int v = cur[x];
                switch (f) {
                case 1: v -= a; break;
                case 2: v -= b; break;
                case 3: v -= (a + b) / 2; break;
                case 4: v -= paeth(a, b, cc); break;
                default: break;
                }
                c[x] = (uint8_t)v;
                sum += (int8_t)c[x] < 0 ? -(int8_t)c[x] : (int8_t)c[x];
            }
            if (best_sum < 0 || sum < best_sum) {
                best_sum = sum;
                best = f;
            }
        }
        raw[(size_t)y * (stride + 1u)] = (uint8_t)best;
        memcpy(raw + (size_t)y * (stride + 1u) + 1u, cand + (size_t)best * stride, stride);
    }
    free(cand);

    memset(&z, 0, sizeof z);
    ob_byte(&z, 0x78);                  /* zlib: deflate, 32 KB window */
    ob_byte(&z, 0x01);
    if (deflate_fixed(&z, raw, rawn) < 0) {
        free(raw);
        free(z.p);
        return -1;
    }
    ob_be32(&z, adler32(raw, rawn));
    free(raw);

    memset(&o, 0, sizeof o);
    ob_bytes(&o, sig, 8);
    ihdr[0] = (uint8_t)(w >> 24); ihdr[1] = (uint8_t)(w >> 16); ihdr[2] = (uint8_t)(w >> 8); ihdr[3] = (uint8_t)w;
    ihdr[4] = (uint8_t)(h >> 24); ihdr[5] = (uint8_t)(h >> 16); ihdr[6] = (uint8_t)(h >> 8); ihdr[7] = (uint8_t)h;
    ihdr[8] = 8;                        /* bit depth */
    ihdr[9] = 2;                        /* truecolour RGB */
    ihdr[10] = 0;
    ihdr[11] = 0;
    ihdr[12] = 0;
    chunk(&o, "IHDR", ihdr, sizeof ihdr);
    chunk(&o, "IDAT", z.p, z.n);
    chunk(&o, "IEND", NULL, 0);
    free(z.p);
    if (z.oom || o.oom) {
        free(o.p);
        return -1;
    }
    *out = o.p;
    *outlen = o.n;
    return 0;
}

int img_write_png(const char *path, const uint16_t *px, int w, int h)
{
    uint8_t *rgb = to_rgb(px, w, h), *png;
    size_t n;
    int r;
    if (!rgb)
        return -1;
    r = png_encode_rgb(rgb, w, h, &png, &n);
    free(rgb);
    if (r < 0) {
        errno = ENOMEM;
        return -1;
    }
    r = write_file(path, png, n, NULL, 0);
    free(png);
    return r;
}

int img_write(const char *path, const uint16_t *px, int w, int h)
{
    size_t n = strlen(path);
    if (n >= 4 && strcasecmp(path + n - 4, ".png") == 0)
        return img_write_png(path, px, w, h);
    return img_write_ppm(path, px, w, h);
}

/* ---- PPM reader ----------------------------------------------------------------------------- */
static int ppm_token(const uint8_t *d, size_t n, size_t *pos, long *out)
{
    long v = 0;
    int any = 0;
    for (;;) {
        while (*pos < n && isspace(d[*pos]))
            (*pos)++;
        if (*pos < n && d[*pos] == '#') {
            while (*pos < n && d[*pos] != '\n' && d[*pos] != '\r')
                (*pos)++;
            continue;
        }
        break;
    }
    while (*pos < n && isdigit(d[*pos])) {
        v = v * 10 + (d[*pos] - '0');
        if (v > 1000000)
            return -1;
        (*pos)++;
        any = 1;
    }
    if (!any)
        return -1;
    *out = v;
    return 0;
}

int img_read_ppm(const char *path, uint8_t **rgb, int *w, int *h, char *err, int errlen)
{
    FILE *f = fopen(path, "rb");
    uint8_t *d = NULL, *o = NULL;
    size_t n = 0, cap = 0, pos = 2, npx, i;
    long W, H, M;
    int ascii;

    *rgb = NULL;
    if (!f) {
        snprintf(err, (size_t)errlen, "%s: %s", path, strerror(errno));
        return -1;
    }
    for (;;) {
        size_t k;
        if (n == cap) {
            uint8_t *q;
            cap = cap ? cap * 2 : 1u << 20;
            q = realloc(d, cap);
            if (!q) {
                fclose(f);
                free(d);
                snprintf(err, (size_t)errlen, "out of memory");
                return -1;
            }
            d = q;
        }
        k = fread(d + n, 1, cap - n, f);
        if (k == 0)
            break;
        n += k;
    }
    fclose(f);
    if (n < 2 || d[0] != 'P' || (d[1] != '6' && d[1] != '3')) {
        snprintf(err, (size_t)errlen, "%s: not a PPM (P6/P3) file", path);
        free(d);
        return -1;
    }
    ascii = d[1] == '3';
    if (ppm_token(d, n, &pos, &W) < 0 || ppm_token(d, n, &pos, &H) < 0 || ppm_token(d, n, &pos, &M) < 0 ||
        W < 1 || H < 1 || W > 32768 || H > 32768 || M < 1 || M > 65535) {
        snprintf(err, (size_t)errlen, "%s: bad PPM header", path);
        free(d);
        return -1;
    }
    npx = (size_t)W * (size_t)H;
    o = malloc(npx * 3u);
    if (!o) {
        snprintf(err, (size_t)errlen, "out of memory");
        free(d);
        return -1;
    }
    if (!ascii) {
        size_t bps = M > 255 ? 2u : 1u;
        pos++;                          /* single whitespace after maxval */
        if (pos > n || n - pos < npx * 3u * bps) {
            snprintf(err, (size_t)errlen, "%s: truncated pixel data", path);
            free(d);
            free(o);
            return -1;
        }
        for (i = 0; i < npx * 3u; i++) {
            long v = bps == 2 ? (long)((d[pos + 2 * i] << 8) | d[pos + 2 * i + 1]) : (long)d[pos + i];
            o[i] = (uint8_t)((v * 255 + M / 2) / M);
        }
    } else {
        for (i = 0; i < npx * 3u; i++) {
            long v;
            if (ppm_token(d, n, &pos, &v) < 0 || v > M) {
                snprintf(err, (size_t)errlen, "%s: bad/truncated P3 data", path);
                free(d);
                free(o);
                return -1;
            }
            o[i] = (uint8_t)((v * 255 + M / 2) / M);
        }
    }
    free(d);
    *rgb = o;
    *w = (int)W;
    *h = (int)H;
    return 0;
}
