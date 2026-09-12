/* ===========================================================================================
 *  psram_layer -- one decoder layer streamed from the bank, in tokens per second
 *
 *  BOARD    Teensy 4.1
 * ======================================================================================== */

#include <Arduino.h>

#include <Arduino.h>

#define B_SS0  (1u << 24)
#define B_CLK  (1u << 25)
#define B_DATA (0xFu << 26)
#define DSHIFT 26
#define B_SS1  (1u << 22)
#define PIN_A 2
#define PIN_B 3
#define PIN_C 4
#define BURST 96

static uint32_t idle;

static inline void bus_out(void) { GPIO9_GDIR |= (B_DATA | B_CLK | B_SS0 | B_SS1); }
static inline void data_in(void) { GPIO9_GDIR &= ~B_DATA; }
static inline uint32_t qbase(void) { return idle & ~B_SS0 & ~B_CLK & ~B_DATA; }
static inline uint32_t sbase(void) { return qbase() | (1u << 28) | (1u << 29); }

template <int S> struct Nop { static inline void go() { __asm__ volatile("nop"); Nop<S - 1>::go(); } };
template <>      struct Nop<0> { static inline void go() { } };
template <int S> static inline void spin(void) { Nop<S>::go(); }

template <int S> static inline void put_nib(uint32_t base, uint8_t nib)
{
    const uint32_t v = base | ((uint32_t)nib << DSHIFT);
    GPIO9_DR = v;
    GPIO9_DR = v | B_CLK;  spin<S>();
    GPIO9_DR = v;          spin<S>();
}

template <int S> static inline uint8_t get_nib(uint32_t base)
{
    GPIO9_DR = base | B_CLK;  spin<S>();
    const uint8_t n = (uint8_t)((GPIO9_PSR >> DSHIFT) & 0xF);
    GPIO9_DR = base;          spin<S>();
    return n;
}

template <int S> static void s_byte(uint32_t base, uint8_t v)
{
    for (int i = 7; i >= 0; i--) {
        const uint32_t d = base | (((v >> i) & 1u) << DSHIFT);
        GPIO9_DR = d;
        GPIO9_DR = d | B_CLK;  spin<S>();
        GPIO9_DR = d;          spin<S>();
    }
}

static void cmd_single(uint8_t c)
{
    bus_out(); const uint32_t b = sbase();
    GPIO9_DR = b; s_byte<8>(b, c); GPIO9_DR = idle; delayMicroseconds(5);
}
static void cmd_quad(uint8_t c)
{
    bus_out(); const uint32_t b = qbase();
    GPIO9_DR = b; put_nib<8>(b, c >> 4); put_nib<8>(b, c & 0xF);
    GPIO9_DR = idle; delayMicroseconds(5);
}

template <int S> static inline void addr_out(uint32_t b, uint32_t a)
{
    put_nib<S>(b, (a >> 20) & 0xF); put_nib<S>(b, (a >> 16) & 0xF);
    put_nib<S>(b, (a >> 12) & 0xF); put_nib<S>(b, (a >>  8) & 0xF);
    put_nib<S>(b, (a >>  4) & 0xF); put_nib<S>(b, (a      ) & 0xF);
}

template <int S> static void wr(uint32_t a, const uint8_t *s, uint32_t len)
{
    while (len) {
        const uint32_t n = (len > BURST) ? BURST : len;
        bus_out(); const uint32_t b = qbase(); GPIO9_DR = b;
        put_nib<S>(b, 0x3); put_nib<S>(b, 0x8); addr_out<S>(b, a);
        for (uint32_t i = 0; i < n; i++) { put_nib<S>(b, s[i] >> 4); put_nib<S>(b, s[i] & 0xF); }
        GPIO9_DR = idle;
        a += n; s += n; len -= n;
    }
}

template <int S> static void rd(uint32_t a, uint8_t *d, uint32_t len)
{
    while (len) {
        const uint32_t n = (len > BURST) ? BURST : len;
        bus_out(); const uint32_t b = qbase(); GPIO9_DR = b;
        put_nib<S>(b, 0xE); put_nib<S>(b, 0xB); addr_out<S>(b, a);
        data_in();
        for (int k = 0; k < 6; k++) { GPIO9_DR = b | B_CLK; spin<S>(); GPIO9_DR = b; spin<S>(); }
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t hi = get_nib<S>(b);
            d[i] = (uint8_t)((hi << 4) | get_nib<S>(b));
        }
        GPIO9_DR = idle; bus_out();
        a += n; d += n; len -= n;
    }
}

static void pick(uint8_t n)
{
    digitalWriteFast(PIN_A, (n >> 0) & 1);
    digitalWriteFast(PIN_B, (n >> 1) & 1);
    digitalWriteFast(PIN_C, (n >> 2) & 1);
    delayMicroseconds(10);
    idle = (GPIO9_DR | B_SS0 | B_SS1 | (1u << 28) | (1u << 29)) & ~B_CLK;
}

static void enter_quad(void)
{
    cmd_quad(0xF5); cmd_single(0x66); cmd_single(0x99);
    delay(2); cmd_single(0x35); delayMicroseconds(50);
}
/* ===========================================================================================
 *  a whole transformer layer, streamed from the bank, in the project's own currency
 *
 *  Everything measured so far is in megabytes and multiply-accumulates. The project's unit is tokens
 *  per second, and the conversion is not a guess: a decoder layer at hidden size d does a fixed
 *  amount of arithmetic against a fixed number of weight bytes, and both are exactly countable.
 *
 *      attention    Q, K, V and the output projection      4 d^2 multiply-accumulates
 *      feed-forward up and gate at 4d, down from 4d       12 d^2
 *      total                                              16 d^2 per token per layer
 *
 *  At four bits a weight that is 8 d^2 bytes of weights per layer. Nothing else in a decode step is
 *  close: the attention over the cache is proportional to context rather than to d^2, and at these
 *  sizes it disappears next to the projections.
 *
 *  So the layer is a known number of bytes and a known number of operations, and this measures the
 *  three times that matter separately:
 *
 *      read only       stream the weights and throw them away. The bus, alone.
 *      compute only    the same byte count of arithmetic, operands already on chip. The kernel, alone.
 *      both            read a block then compute it, which is what actually has to happen.
 *
 *  THE POINT OF SPLITTING IT. Elsewhere this project's cost model adds read and compute as
 *  reciprocals, which is right for a hardware controller: the transfer happens in the background and
 *  the slower of the two sets the pace. On a bit-banged bus the processor is the transfer, so nothing
 *  overlaps and the two times add outright. Measuring all three says which model applies and by how
 *  much, instead of assuming.
 *
 *  Both halves use the SMLAD kernel from psram_matrix, because comparing a hand-optimised bus against
 *  an unoptimised kernel would flatter the bus.
 * ======================================================================================== */

#define BLK   (8u * 1024u)
#define LAYERS 24u                      /* a typical small decoder, for the token-rate arithmetic */

static uint8_t  stage[BLK] __attribute__((aligned(32)));
static uint8_t  onchip[BLK] __attribute__((aligned(32)));
static int8_t   xvec[2048];
static uint32_t xpack[256 * 4];
static int32_t  xsum_all;

static inline uint32_t sxtb16(uint32_t x)
{
    uint32_t r; __asm__("sxtb16 %0, %1" : "=r"(r) : "r"(x)); return r;
}
static inline uint32_t sxtb16_ror8(uint32_t x)
{
    uint32_t r; __asm__("sxtb16 %0, %1, ror #8" : "=r"(r) : "r"(x)); return r;
}
static inline int32_t smlad(uint32_t a, uint32_t b, int32_t acc)
{
    int32_t r; __asm__("smlad %0, %1, %2, %3" : "=r"(r) : "r"(a), "r"(b), "r"(acc)); return r;
}

static void build_xpack(void)
{
    for (uint32_t g = 0; g < 256; g++) {
        const int8_t *a = &xvec[g * 8];
        xpack[g*4+0] = ((uint32_t)(a[0] & 0xFFFF))       | ((uint32_t)(a[4] & 0xFFFF) << 16);
        xpack[g*4+1] = ((uint32_t)(a[2] & 0xFFFF))       | ((uint32_t)(a[6] & 0xFFFF) << 16);
        xpack[g*4+2] = ((uint32_t)(a[1] & 0xFFFF))       | ((uint32_t)(a[5] & 0xFFFF) << 16);
        xpack[g*4+3] = ((uint32_t)(a[3] & 0xFFFF))       | ((uint32_t)(a[7] & 0xFFFF) << 16);
    }
    xsum_all = 0;
    for (int i = 0; i < 2048; i++) xsum_all += xvec[i];
}

/* the kernel from psram_matrix, 2.57 cycles per multiply-accumulate */
static inline int32_t mac4(const uint8_t *w, uint32_t n)
{
    int32_t a0 = 0, a1 = 0;
    uint32_t g = 0;
    for (uint32_t i = 0; i + 8 <= n; i += 8) {
        const uint32_t *P = &xpack[g * 4];
        const uint32_t v0 = *(const uint32_t *)(w + i);
        const uint32_t v1 = *(const uint32_t *)(w + i + 4);
        const uint32_t L0 = v0 & 0x0F0F0F0Fu, H0 = (v0 >> 4) & 0x0F0F0F0Fu;
        const uint32_t L1 = v1 & 0x0F0F0F0Fu, H1 = (v1 >> 4) & 0x0F0F0F0Fu;
        a0 = smlad(sxtb16(L0),      P[0], a0);
        a0 = smlad(sxtb16_ror8(L0), P[1], a0);
        a0 = smlad(sxtb16(H0),      P[2], a0);
        a0 = smlad(sxtb16_ror8(H0), P[3], a0);
        a1 = smlad(sxtb16(L1),      P[4], a1);
        a1 = smlad(sxtb16_ror8(L1), P[5], a1);
        a1 = smlad(sxtb16(H1),      P[6], a1);
        a1 = smlad(sxtb16_ror8(H1), P[7], a1);
        g = (g + 2) & 255u;
    }
    return a0 + a1 - 8 * (int32_t)(n / 1024u) * xsum_all;
}

/* ---------------------------------------------------------------------------------------------
 *  and the same layer in the format a real model actually uses
 *
 *  Everything above uses a bare 4-bit kernel: one scale for everything, a constant zero point, integer
 *  output. It is the right thing to bound the bus with, and it is not what a GGUF file contains. Q4_K
 *  puts 256 weights in 144 bytes with a float scale and minimum per block and six-bit scales per
 *  32-weight sub-block, and docs/43 estimated its layer time by scaling the bare figure rather than
 *  measuring it.
 *
 *  This measures it. Same three timings -- read alone, compute alone, both -- over real Q4_K blocks
 *  streamed from the bank, with the kernel from shared/gguf_dot.c rather than a local copy of the idea.
 *
 *  The chunk is 56 blocks, 8064 bytes, because a chunk has to hold whole blocks and 8064 is the largest
 *  multiple of 144 that fits the staging buffer. A chunk that split a block would hand the kernel a
 *  header that belongs to the previous read.
 * ------------------------------------------------------------------------------------------ */
extern "C" {
float gguf_dot_q4k_presum(const void *raw, const int8_t *xq, const float *xs,
                          const int32_t *xsum, uint64_t n);
void  gguf_act_sums(const int8_t *xq, uint64_t n, int32_t *xsum);
void  gguf_quantize_act(const float *x, uint64_t n, int8_t *xq, float *xs);
const char *gguf_dot_kernel(void);
}

#define Q4K_BYTES 144u
#define Q4K_NW    256u
#define QCHUNK    (Q4K_BYTES * 56u)          /* 8064: whole blocks, fits the staging buffer */

static int8_t   q_xq[Q4K_NW];
static float    q_xs[Q4K_NW / 32];
static int32_t  q_xsum[Q4K_NW / 32];
static volatile float fsink;

static volatile int32_t sink;

static uint32_t t_read(uint32_t bytes)
{
    const uint32_t t0 = ARM_DWT_CYCCNT;
    for (uint32_t off = 0; off < bytes; off += BLK) rd<10>(off, stage, BLK);
    return ARM_DWT_CYCCNT - t0;
}

static uint32_t t_compute(uint32_t bytes)
{
    const uint32_t t0 = ARM_DWT_CYCCNT;
    int32_t a = 0;
    for (uint32_t off = 0; off < bytes; off += BLK) a += mac4(onchip, BLK);
    const uint32_t c = ARM_DWT_CYCCNT - t0;
    sink = a;
    return c;
}

static uint32_t t_both(uint32_t bytes)
{
    const uint32_t t0 = ARM_DWT_CYCCNT;
    int32_t a = 0;
    for (uint32_t off = 0; off < bytes; off += BLK) { rd<10>(off, stage, BLK); a += mac4(stage, BLK); }
    const uint32_t c = ARM_DWT_CYCCNT - t0;
    sink = a;
    return c;
}

static inline float ms(uint32_t c) { return 1000.0f * (float)c / (float)F_CPU_ACTUAL; }

/* one layer's worth of Q4_K weights: 16 d^2 weights is 16 d^2 * 144/256 = 9 d^2 bytes */
static void layer_q4k(uint32_t d)
{
    const uint32_t weights = 16u * d * d;
    const uint32_t blocks  = weights / Q4K_NW;
    const uint32_t bytes   = blocks * Q4K_BYTES;

    if (bytes > 8u * 1024u * 1024u) {
        Serial.print(F("\n  hidden size ")); Serial.print(d);
        Serial.println(F(" in Q4_K needs more than the 8 MB the bank can hold"));
        return;
    }

    /* lay the blocks down once. The contents are arbitrary -- the cost does not depend on the values,
     * and the activation is quantized properly so the kernel walks the path the runtime walks. */
    for (uint32_t off = 0; off < bytes; off += QCHUNK) {
        const uint32_t n = (bytes - off > QCHUNK) ? QCHUNK : (bytes - off);
        for (uint32_t i = 0; i < n; i++) stage[i] = (uint8_t)((off + i) * 0x9Du + 0x3Bu);
        wr<6>(off, stage, n);
    }

    /* read only */
    uint32_t t0 = ARM_DWT_CYCCNT;
    for (uint32_t off = 0; off < bytes; off += QCHUNK) {
        const uint32_t n = (bytes - off > QCHUNK) ? QCHUNK : (bytes - off);
        rd<10>(off, stage, n);
    }
    const float r = ms(ARM_DWT_CYCCNT - t0);

    /* compute only, over one chunk already on chip, repeated to the same block count */
    memcpy(onchip, stage, QCHUNK > sizeof(onchip) ? sizeof(onchip) : QCHUNK);
    const uint32_t per = (QCHUNK > sizeof(onchip) ? (uint32_t)sizeof(onchip) : QCHUNK) / Q4K_BYTES;
    float acc = 0.0f;
    t0 = ARM_DWT_CYCCNT;
    for (uint32_t done = 0; done < blocks; done += per)
        for (uint32_t i = 0; i < per && done + i < blocks; i++)
            acc += gguf_dot_q4k_presum(onchip + i * Q4K_BYTES, q_xq, q_xs, q_xsum, Q4K_NW);
    const float c = ms(ARM_DWT_CYCCNT - t0);

    /* both, which is what has to happen */
    t0 = ARM_DWT_CYCCNT;
    for (uint32_t off = 0; off < bytes; off += QCHUNK) {
        const uint32_t n = (bytes - off > QCHUNK) ? QCHUNK : (bytes - off);
        rd<10>(off, stage, n);
        for (uint32_t i = 0; i + Q4K_BYTES <= n; i += Q4K_BYTES)
            acc += gguf_dot_q4k_presum(stage + i, q_xq, q_xs, q_xsum, Q4K_NW);
    }
    const float b = ms(ARM_DWT_CYCCNT - t0);
    fsink = acc;

    Serial.println();
    Serial.print(F("  hidden size ")); Serial.print(d);
    Serial.print(F(" in real Q4_K:  ")); Serial.print(blocks);
    Serial.print(F(" blocks, ")); Serial.print(bytes / 1024u);
    Serial.println(F(" kB per layer"));
    Serial.print(F("    read only        ")); Serial.print(r, 2);
    Serial.print(F(" ms    ")); Serial.print((float)bytes / r / 1000.0f, 2);
    Serial.println(F(" MB/s"));
    Serial.print(F("    compute only     ")); Serial.print(c, 2);
    Serial.print(F(" ms    ")); Serial.print((float)bytes / c / 1000.0f, 2);
    Serial.println(F(" MB/s"));
    Serial.print(F("    both, per layer  ")); Serial.print(b, 2);
    Serial.print(F(" ms    sum of the two is ")); Serial.print(r + c, 2);
    Serial.println(F(" ms"));
    Serial.print(F("    measured / sum = ")); Serial.print(b / (r + c), 3);
    Serial.println(b < 0.8f * (r + c) ? F("   they overlap") : F("   they add, as before"));
    Serial.print(F("    -> ")); Serial.print(1000.0f / (b * LAYERS), 3);
    Serial.print(F(" tokens/s for a ")); Serial.print(LAYERS);
    Serial.println(F("-layer model, measured rather than scaled"));
}

static void layer(uint32_t d)
{
    const uint32_t macs  = 16u * d * d;          /* per token, per layer */
    const uint32_t bytes = macs / 2u;            /* two four-bit weights to a byte */

    /* lay the weights down once, then never write again: this is decode, and decode only reads */
    for (uint32_t off = 0; off < bytes; off += BLK) {
        for (uint32_t i = 0; i < BLK; i++) stage[i] = (uint8_t)((off + i) * 0x9Du + 0x3Bu);
        wr<6>(off, stage, BLK);
    }
    memcpy(onchip, stage, BLK);

    const float r = ms(t_read(bytes));
    const float c = ms(t_compute(bytes));
    const float b = ms(t_both(bytes));

    Serial.println();
    Serial.print(F("  hidden size ")); Serial.print(d);
    Serial.print(F(":  ")); Serial.print(macs / 1000u);
    Serial.print(F("k multiply-accumulates, ")); Serial.print(bytes / 1024u);
    Serial.println(F(" kB of 4-bit weights per layer"));

    Serial.print(F("    read only        ")); Serial.print(r, 2);
    Serial.print(F(" ms    ")); Serial.print((float)bytes / r / 1000.0f, 2);
    Serial.println(F(" MB/s"));
    Serial.print(F("    compute only     ")); Serial.print(c, 2);
    Serial.print(F(" ms    ")); Serial.print((float)macs / c / 1000.0f, 2);
    Serial.println(F(" MMAC/s"));
    Serial.print(F("    both, per layer  ")); Serial.print(b, 2);
    Serial.println(F(" ms"));

    /* which cost model is this bus actually obeying */
    Serial.print(F("    sum of the two is ")); Serial.print(r + c, 2);
    Serial.print(F(" ms, the slower alone is ")); Serial.print(r > c ? r : c, 2);
    Serial.println(F(" ms"));
    Serial.print(F("    measured / sum = ")); Serial.print(b / (r + c), 3);
    Serial.print(F(", measured / slower = ")); Serial.println(b / (r > c ? r : c), 3);
    Serial.println(b < 0.8f * (r + c)
        ? F("    they overlap: the reciprocal cost model applies")
        : F("    they add: nothing overlaps, because the processor IS the transfer"));

    /* and the only number the project actually cares about */
    Serial.print(F("    -> ")); Serial.print(1000.0f / b, 1);
    Serial.print(F(" layers/s, so ")); Serial.print(1000.0f / (b * LAYERS), 3);
    Serial.print(F(" tokens/s for a ")); Serial.print(LAYERS);
    Serial.println(F("-layer model on one Teensy"));
    Serial.print(F("       if the controller were usable at 33 MB/s and overlapped: "));
    {
        const float rr = (float)bytes / 33.0f / 1000.0f;      /* ms at 33 MB/s */
        const float bb = rr > c ? rr : c;
        Serial.print(1000.0f / (bb * LAYERS), 3);
        Serial.println(F(" tokens/s"));
    }
}

void setup()
{
    Serial.begin(115200);
    pinMode(PIN_A, OUTPUT); pinMode(PIN_B, OUTPUT); pinMode(PIN_C, OUTPUT);
    for (int p = 48; p <= 54; p++) pinMode(p, OUTPUT);
    GPIO9_DR |= (B_SS0 | B_SS1);
    bus_out();
    pick(7);
    ARM_DEMCR |= ARM_DEMCR_TRCENA;
    ARM_DWT_CTRL |= ARM_DWT_CTRL_CYCCNTENA;
    for (int i = 0; i < 2048; i++) xvec[i] = (int8_t)(((i * 37) & 0x7F) - 64);
    build_xpack();

    /* a plausible activation, quantized the way the runtime quantizes it, and its per-32 sums hoisted
     * out once -- which is the whole point of gguf_act_sums and is what a matrix-vector product does */
    static float act[Q4K_NW];
    for (uint32_t i = 0; i < Q4K_NW; i++) act[i] = ((float)(i % 37) - 18.0f) * 0.05f;
    gguf_quantize_act(act, Q4K_NW, q_xq, q_xs);
    gguf_act_sums(q_xq, Q4K_NW, q_xsum);
}

void loop()
{
    Serial.println();
    Serial.println(F("=================================================================="));
    Serial.println(F("  one decoder layer, 4-bit weights, streamed from the PSRAM bank"));
    Serial.println(F("=================================================================="));

    pick(7);
    enter_quad();

    layer(256);
    layer(512);
    layer(1024);

    Serial.print(F("\n  --- and the same thing in the format a real model uses. kernel: "));
    Serial.print(gguf_dot_kernel());
    Serial.println(F(" ---"));
    layer_q4k(256);
    layer_q4k(512);

    Serial.println(F("\n--- what this says about the architecture ---"));
    Serial.println(F("  The weights of one layer have to cross the bus once per token and there is"));
    Serial.println(F("  no reuse to hide it behind, because decode is one token at a time. So the"));
    Serial.println(F("  token rate is set by bytes per layer divided by bus rate, and the kernel"));
    Serial.println(F("  only matters while it is slower than the bus. In real Q4_K the arithmetic"));
    Serial.println(F("  is about a fifth of the layer and the bus is the rest, so optimising the"));
    Serial.println(F("  kernel further buys little and widening the bus buys everything."));
    Serial.println(F("  Q4_K also needs 9 d^2 bytes a layer where a bare 4-bit kernel needs 8: the"));
    Serial.println(F("  scales and minimums are bus traffic that carries no weights, a 12.5% tax"));
    Serial.println(F("  on the scarcest thing in the machine."));

    Serial.println(F("\n=== repeating in 15 s ==="));
    delay(15000);
}
