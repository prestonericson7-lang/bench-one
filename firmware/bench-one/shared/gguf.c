/* gguf.c -- see gguf.h for what this is and why the quantization formats are the hard part. */

#include "gguf.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32) && !defined(GGUF_NO_MMAP)
  #include <sys/mman.h>
  #include <sys/stat.h>
  #include <unistd.h>
#endif
#if defined(_WIN32)
  #define SEEK64(f, off) _fseeki64((f), (long long)(off), SEEK_SET)
#else
  #define SEEK64(f, off) fseeko((f), (off_t)(off), SEEK_SET)
#endif

/* ---------------------------------------------------------------------------------------------
 *  half precision
 * ------------------------------------------------------------------------------------------ */

/* Half precision and the Q4_K scale unpacker now live in gguf_bits.c, so that a target with no
 * filesystem can link the arithmetic without linking stdio. Same one copy, used by both. */

/* ---------------------------------------------------------------------------------------------
 *  type table
 * ------------------------------------------------------------------------------------------ */

typedef struct { const char *name; uint32_t blk; uint32_t bytes; } tinfo_t;

static const tinfo_t TYPES[16] = {
    { "F32",   1,   4 }, { "F16",   1,   2 }, { "Q4_0", 32,  18 }, { "Q4_1", 32,  20 },
    { "?4",    0,   0 }, { "?5",    0,   0 }, { "Q5_0", 32,  22 }, { "Q5_1", 32,  24 },
    { "Q8_0", 32,  34 }, { "Q8_1", 32,  36 }, { "Q2_K", 256, 84 }, { "Q3_K", 256, 110 },
    { "Q4_K", 256, 144 }, { "Q5_K", 256, 176 }, { "Q6_K", 256, 210 }, { "Q8_K", 256, 292 }
};

const char *gguf_type_name(uint32_t type)
{
    return (type < 16 && TYPES[type].blk) ? TYPES[type].name : "UNKNOWN";
}

/* ---------------------------------------------------------------------------------------------
 *  header parsing
 * ------------------------------------------------------------------------------------------ */

static int rd(FILE *f, void *p, size_t n) { return fread(p, 1, n, f) == n ? 0 : -1; }

static int rd_u32(FILE *f, uint32_t *v) { return rd(f, v, 4); }
static int rd_u64(FILE *f, uint64_t *v) { return rd(f, v, 8); }

static char *rd_str(FILE *f)
{
    uint64_t n;
    if (rd_u64(f, &n) || n > (1u << 28)) return NULL;
    char *s = (char *)malloc((size_t)n + 1);
    if (!s) return NULL;
    if (n && rd(f, s, (size_t)n)) { free(s); return NULL; }
    s[n] = 0;
    return s;
}

static size_t scalar_width(uint32_t t)
{
    switch (t) {
    case GGUF_U8: case GGUF_I8: case GGUF_BOOL: return 1;
    case GGUF_U16: case GGUF_I16:               return 2;
    case GGUF_U32: case GGUF_I32: case GGUF_F32:return 4;
    case GGUF_U64: case GGUF_I64: case GGUF_F64:return 8;
    default:                                    return 0;
    }
}

static int rd_scalar(FILE *f, uint32_t t, gguf_kv *kv)
{
    switch (t) {
    case GGUF_U8:  { uint8_t  v; if (rd(f,&v,1)) return -1; kv->i = v; return 0; }
    case GGUF_I8:  { int8_t   v; if (rd(f,&v,1)) return -1; kv->i = v; return 0; }
    case GGUF_BOOL:{ uint8_t  v; if (rd(f,&v,1)) return -1; kv->i = v ? 1 : 0; return 0; }
    case GGUF_U16: { uint16_t v; if (rd(f,&v,2)) return -1; kv->i = v; return 0; }
    case GGUF_I16: { int16_t  v; if (rd(f,&v,2)) return -1; kv->i = v; return 0; }
    case GGUF_U32: { uint32_t v; if (rd(f,&v,4)) return -1; kv->i = v; return 0; }
    case GGUF_I32: { int32_t  v; if (rd(f,&v,4)) return -1; kv->i = v; return 0; }
    case GGUF_U64: { uint64_t v; if (rd(f,&v,8)) return -1; kv->i = (int64_t)v; return 0; }
    case GGUF_I64: { int64_t  v; if (rd(f,&v,8)) return -1; kv->i = v; return 0; }
    case GGUF_F32: { float    v; if (rd(f,&v,4)) return -1; kv->f = v; kv->i = (int64_t)v; return 0; }
    case GGUF_F64: { double   v; if (rd(f,&v,8)) return -1; kv->f = v; kv->i = (int64_t)v; return 0; }
    case GGUF_STR: { char *s = rd_str(f); if (!s) return -1; kv->s = s; return 0; }
    default: return -1;
    }
}

int gguf_open(gguf_t *g, const char *path)
{
    memset(g, 0, sizeof(*g));
    g->f = fopen(path, "rb");
    if (!g->f) { snprintf(g->err, sizeof(g->err), "cannot open %s", path); return -1; }

    char magic[4];
    if (rd(g->f, magic, 4) || memcmp(magic, "GGUF", 4) != 0) {
        snprintf(g->err, sizeof(g->err), "not a GGUF file");
        return -1;
    }
    if (rd_u32(g->f, &g->version) || rd_u64(g->f, &g->n_tensors) || rd_u64(g->f, &g->n_kv)) {
        snprintf(g->err, sizeof(g->err), "truncated header");
        return -1;
    }
    if (g->n_tensors > 100000u || g->n_kv > 100000u) {
        snprintf(g->err, sizeof(g->err), "implausible counts: %llu tensors, %llu kv",
                 (unsigned long long)g->n_tensors, (unsigned long long)g->n_kv);
        return -1;
    }

    g->kv = (gguf_kv *)calloc((size_t)g->n_kv, sizeof(gguf_kv));
    g->t  = (gguf_tensor *)calloc((size_t)g->n_tensors, sizeof(gguf_tensor));
    if (!g->kv || !g->t) { snprintf(g->err, sizeof(g->err), "out of memory"); return -1; }

    for (uint64_t i = 0; i < g->n_kv; i++) {
        gguf_kv *kv = &g->kv[i];
        kv->key = rd_str(g->f);
        if (!kv->key || rd_u32(g->f, &kv->type)) {
            snprintf(g->err, sizeof(g->err), "bad metadata at entry %llu", (unsigned long long)i);
            return -1;
        }
        kv->n = 1;
        if (kv->type != GGUF_ARR) {
            if (rd_scalar(g->f, kv->type, kv)) {
                snprintf(g->err, sizeof(g->err), "bad value for %s", kv->key);
                return -1;
            }
            continue;
        }
        if (rd_u32(g->f, &kv->arr_type) || rd_u64(g->f, &kv->n)) {
            snprintf(g->err, sizeof(g->err), "bad array header for %s", kv->key);
            return -1;
        }
        if (kv->arr_type == GGUF_STR) {
            /* The tokenizer lives here: 151k strings for this model. Kept, because a runtime that
             * cannot turn a token id back into text cannot be checked by reading its output. */
            kv->sv = (char **)calloc((size_t)kv->n, sizeof(char *));
            if (!kv->sv) { snprintf(g->err, sizeof(g->err), "out of memory"); return -1; }
            for (uint64_t j = 0; j < kv->n; j++) {
                kv->sv[j] = rd_str(g->f);
                if (!kv->sv[j]) {
                    snprintf(g->err, sizeof(g->err), "bad string %llu in %s",
                             (unsigned long long)j, kv->key);
                    return -1;
                }
            }
        } else {
            const size_t w = scalar_width(kv->arr_type);
            if (!w) {
                snprintf(g->err, sizeof(g->err), "array of type %u in %s", kv->arr_type, kv->key);
                return -1;
            }
            kv->av = malloc((size_t)kv->n * w);
            if (!kv->av) { snprintf(g->err, sizeof(g->err), "out of memory"); return -1; }
            if (kv->n && rd(g->f, kv->av, (size_t)kv->n * w)) {
                snprintf(g->err, sizeof(g->err), "truncated array in %s", kv->key);
                return -1;
            }
        }
    }

    for (uint64_t i = 0; i < g->n_tensors; i++) {
        gguf_tensor *t = &g->t[i];
        t->name = rd_str(g->f);
        if (!t->name || rd_u32(g->f, &t->n_dims) || t->n_dims > 4) {
            snprintf(g->err, sizeof(g->err), "bad tensor %llu", (unsigned long long)i);
            return -1;
        }
        for (uint32_t d = 0; d < t->n_dims; d++) {
            if (rd_u64(g->f, &t->dims[d])) {
                snprintf(g->err, sizeof(g->err), "bad dims for %s", t->name);
                return -1;
            }
        }
        for (uint32_t d = t->n_dims; d < 4; d++) t->dims[d] = 1;
        if (rd_u32(g->f, &t->type) || rd_u64(g->f, &t->offset)) {
            snprintf(g->err, sizeof(g->err), "bad descriptor for %s", t->name);
            return -1;
        }
    }

    /* The blob is aligned, and the padding is NOT optional: get this wrong and every tensor is
     * offset by a few bytes, which dequantizes to noise rather than failing. */
    const int64_t align = gguf_int(g, "general.alignment", 32);
#if defined(_WIN32)
    const uint64_t here = (uint64_t)_ftelli64(g->f);
#else
    const uint64_t here = (uint64_t)ftello(g->f);
#endif
    g->data_start = here;
    if (align > 0 && (here % (uint64_t)align))
        g->data_start = here + (uint64_t)align - (here % (uint64_t)align);
    /* Map the file read-only where the platform allows. Loads then take pointers into the map and
     * the model is paged in from the file as it is used, so a 256 MB host runs a 1.9 GB model the
     * way the Teensy streams its card. A failed map is not an error: reads still work. */
#if !defined(_WIN32) && !defined(GGUF_NO_MMAP)
    {
        struct stat st;
        if (fstat(fileno(g->f), &st) == 0 && st.st_size > 0 && (uint64_t)st.st_size <= (uint64_t)(size_t)-1) {
            void *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fileno(g->f), 0);
            if (p != MAP_FAILED) { g->map = (uint8_t *)p; g->map_len = (uint64_t)st.st_size; }
        }
    }
#endif
    return 0;
}

const uint8_t *gguf_raw_ptr(const gguf_t *g, const gguf_tensor *t)
{
    if (!g->map || !t) return NULL;
    const uint64_t off = g->data_start + t->offset;
    const uint64_t nb = gguf_nbytes(t);
    if (!nb || off + nb > g->map_len) return NULL;
    return g->map + off;
}

void gguf_close(gguf_t *g)
{
#if !defined(_WIN32) && !defined(GGUF_NO_MMAP)
    if (g->map) munmap(g->map, (size_t)g->map_len);
#endif
    if (g->kv) {
        for (uint64_t i = 0; i < g->n_kv; i++) {
            free(g->kv[i].key);
            free(g->kv[i].s);
            free(g->kv[i].av);
            if (g->kv[i].sv) {
                for (uint64_t j = 0; j < g->kv[i].n; j++) free(g->kv[i].sv[j]);
                free(g->kv[i].sv);
            }
        }
        free(g->kv);
    }
    if (g->t) {
        for (uint64_t i = 0; i < g->n_tensors; i++) free(g->t[i].name);
        free(g->t);
    }
    if (g->f) fclose(g->f);
    memset(g, 0, sizeof(*g));
}

/* ---------------------------------------------------------------------------------------------
 *  lookup
 * ------------------------------------------------------------------------------------------ */

const gguf_kv *gguf_find(const gguf_t *g, const char *key)
{
    for (uint64_t i = 0; i < g->n_kv; i++)
        if (g->kv[i].key && strcmp(g->kv[i].key, key) == 0) return &g->kv[i];
    return NULL;
}

int64_t gguf_int(const gguf_t *g, const char *key, int64_t dflt)
{
    const gguf_kv *k = gguf_find(g, key);
    return k ? k->i : dflt;
}

double gguf_flt(const gguf_t *g, const char *key, double dflt)
{
    const gguf_kv *k = gguf_find(g, key);
    if (!k) return dflt;
    return (k->type == GGUF_F32 || k->type == GGUF_F64) ? k->f : (double)k->i;
}

const char *gguf_str(const gguf_t *g, const char *key, const char *dflt)
{
    const gguf_kv *k = gguf_find(g, key);
    return (k && k->s) ? k->s : dflt;
}

static void arch_key(const gguf_t *g, const char *suffix, char *out, size_t n)
{
    const char *a = gguf_str(g, "general.architecture", "llama");
    snprintf(out, n, "%s.%s", a, suffix);
}

int64_t gguf_arch_int(const gguf_t *g, const char *suffix, int64_t dflt)
{
    char k[192]; arch_key(g, suffix, k, sizeof(k));
    return gguf_int(g, k, dflt);
}

double gguf_arch_flt(const gguf_t *g, const char *suffix, double dflt)
{
    char k[192]; arch_key(g, suffix, k, sizeof(k));
    return gguf_flt(g, k, dflt);
}

const gguf_tensor *gguf_tensor_find(const gguf_t *g, const char *name)
{
    for (uint64_t i = 0; i < g->n_tensors; i++)
        if (g->t[i].name && strcmp(g->t[i].name, name) == 0) return &g->t[i];
    return NULL;
}

const gguf_tensor *gguf_tensor_findf(const gguf_t *g, const char *fmt, ...)
{
    char name[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(name, sizeof(name), fmt, ap);
    va_end(ap);
    return gguf_tensor_find(g, name);
}

uint64_t gguf_nelem(const gguf_tensor *t)
{
    uint64_t n = 1;
    for (uint32_t d = 0; d < 4; d++) n *= t->dims[d] ? t->dims[d] : 1;
    return n;
}

uint64_t gguf_nbytes(const gguf_tensor *t)
{
    const uint64_t n = gguf_nelem(t);
    if (t->type >= 16 || !TYPES[t->type].blk) return 0;
    const tinfo_t *ti = &TYPES[t->type];
    return (n / ti->blk) * ti->bytes;
}

/* ---------------------------------------------------------------------------------------------
 *  dequantization -- the part where a wrong bit produces fluent nonsense
 * ------------------------------------------------------------------------------------------ */

/* The dequantizers and gguf_dequant now live in gguf_bits.c, with the rest of the format arithmetic,
 * so a target with no filesystem can unpack a block without linking one. */

/* ---------------------------------------------------------------------------------------------
 *  reading
 * ------------------------------------------------------------------------------------------ */

uint64_t gguf_row_bytes(const gguf_tensor *t)
{
    if (t->type >= 16 || !TYPES[t->type].blk) return 0;
    return (t->dims[0] / TYPES[t->type].blk) * TYPES[t->type].bytes;
}

int gguf_read_raw_rows(gguf_t *g, const gguf_tensor *t, uint64_t row0, uint64_t n_rows, void *out)
{
    const uint64_t rb = gguf_row_bytes(t);
    const uint64_t rows = t->dims[1] ? t->dims[1] : 1;
    if (!rb) {
        snprintf(g->err, sizeof(g->err), "%s: type %u has no known size", t->name, t->type);
        return -1;
    }
    if (row0 + n_rows > rows) {
        snprintf(g->err, sizeof(g->err), "%s: rows %llu..%llu outside %llu", t->name,
                 (unsigned long long)row0, (unsigned long long)(row0 + n_rows),
                 (unsigned long long)rows);
        return -1;
    }
    if (SEEK64(g->f, g->data_start + t->offset + row0 * rb) != 0 ||
        fread(out, 1, (size_t)(rb * n_rows), g->f) != (size_t)(rb * n_rows)) {
        snprintf(g->err, sizeof(g->err), "%s: short read of rows %llu..%llu", t->name,
                 (unsigned long long)row0, (unsigned long long)(row0 + n_rows));
        return -1;
    }
    return 0;
}

int gguf_read_raw(gguf_t *g, const gguf_tensor *t, void *out)
{
    const uint64_t nb = gguf_nbytes(t);
    if (!nb) {
        snprintf(g->err, sizeof(g->err), "%s: type %u has no known size", t->name, t->type);
        return -1;
    }
    if (SEEK64(g->f, g->data_start + t->offset) != 0 ||
        fread(out, 1, (size_t)nb, g->f) != (size_t)nb) {
        snprintf(g->err, sizeof(g->err), "%s: short read of %.1f MB", t->name, nb / 1048576.0);
        return -1;
    }
    return 0;
}

int gguf_read_f32(gguf_t *g, const gguf_tensor *t, float *out)
{
    return gguf_read_rows_f32(g, t, 0, t->dims[1] ? t->dims[1] : 1, out);
}

int gguf_read_rows_f32(gguf_t *g, const gguf_tensor *t, uint64_t row0, uint64_t n_rows, float *out)
{
    if (t->type >= 16 || !TYPES[t->type].blk) {
        snprintf(g->err, sizeof(g->err), "%s: type %u not supported", t->name, t->type);
        return -1;
    }
    const tinfo_t *ti = &TYPES[t->type];
    const uint64_t cols = t->dims[0];
    const uint64_t rows = t->dims[1] ? t->dims[1] : 1;

    if (row0 + n_rows > rows) {
        snprintf(g->err, sizeof(g->err), "%s: rows %llu..%llu outside %llu", t->name,
                 (unsigned long long)row0, (unsigned long long)(row0 + n_rows),
                 (unsigned long long)rows);
        return -1;
    }
    /* A row has to land on a block boundary or it cannot be read independently, and every shape in
     * a real model does: ggml quantizes along the row. Checked anyway, because the failure mode of
     * not checking is reading the right number of bytes from the wrong place. */
    if (cols % ti->blk) {
        snprintf(g->err, sizeof(g->err), "%s: %llu columns is not a multiple of block %u",
                 t->name, (unsigned long long)cols, ti->blk);
        return -1;
    }

    const uint64_t row_bytes = (cols / ti->blk) * ti->bytes;
    const uint64_t want = row_bytes * n_rows;

    uint8_t *raw = (uint8_t *)malloc((size_t)want);
    if (!raw) {
        snprintf(g->err, sizeof(g->err), "%s: cannot allocate %.1f MB", t->name, want / 1048576.0);
        return -1;
    }
    if (SEEK64(g->f, g->data_start + t->offset + row0 * row_bytes) != 0 ||
        fread(raw, 1, (size_t)want, g->f) != (size_t)want) {
        snprintf(g->err, sizeof(g->err), "%s: short read at row %llu", t->name,
                 (unsigned long long)row0);
        free(raw);
        return -1;
    }
    const int rc = gguf_dequant(t->type, raw, cols * n_rows, out);
    if (rc) snprintf(g->err, sizeof(g->err), "%s: cannot dequantize %s", t->name,
                     gguf_type_name(t->type));
    free(raw);
    return rc;
}
