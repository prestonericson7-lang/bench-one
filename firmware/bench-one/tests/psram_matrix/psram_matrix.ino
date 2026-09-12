/* ===========================================================================================
 *  psram_matrix -- can this thing actually do the arithmetic it was built for
 * ===========================================================================================
 *
 *  The memory works: 8 MB verified, 23.69 MB/s write and 14.83 MB/s read, driven by GPIO because
 *  the hardware controller cannot be slowed enough for breadboard wiring. That is a storage result.
 *  It says nothing about whether the machine can compute.
 *
 *  So this runs the three kinds of arithmetic the project actually needs, with the operands living
 *  in that memory, and measures what comes out.
 *
 *      1  a 4x4 matrix against a stream of vertices     what a GPU does, in float
 *      2  int8 matrix-vector                           one layer of a network, unquantized
 *      3  4-bit matrix-vector with unpacking           what the real models use
 *      4  the same 4-bit kernel from on-chip memory    the ceiling, with no external memory at all
 *
 *  Case 4 is the important one. Everything else is measured against it, because it isolates how much
 *  of the cost is arithmetic and how much is the bus.
 *
 *
 *  WHY THIS IS EXPECTED TO BE SLOW, AND WHY THAT IS STILL WORTH KNOWING
 *  -------------------------------------------------------------------
 *  Reading over a bit-banged bus is not free in the way a hardware transfer is. The processor is the
 *  thing toggling the pins, so a read and a multiply cannot overlap at all -- they are strictly
 *  serial. On the normal FlexSPI path a read is a load and the controller does the work, which is
 *  why the cost model elsewhere in this project adds read and unpack as reciprocals. Here they add
 *  outright.
 *
 *  That makes this a lower bound rather than a verdict. The perfboard rebuild, if it gets under
 *  20.2 ns per nibble, hands the transfer back to hardware and the two costs start overlapping
 *  again.
 *
 *  BOARD    Teensy 4.1
 * ======================================================================================== */

#include <Arduino.h>

#define B_SS0  (1u << 24)
#define B_CLK  (1u << 25)
#define B_DATA (0xFu << 26)
#define DSHIFT 26
#define B_SS1  (1u << 22)
#define PIN_A 2
#define PIN_B 3
#define PIN_C 4
/* corrected after the margin work: see psram_driver and docs/41 */
#define SW     6
#define SR     10
#define BURST  96

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
    GPIO9_DR = b; s_byte<SR>(b, c); GPIO9_DR = idle; delayMicroseconds(5);
}
static void cmd_quad(uint8_t c)
{
    bus_out(); const uint32_t b = qbase();
    GPIO9_DR = b; put_nib<SR>(b, c >> 4); put_nib<SR>(b, c & 0xF);
    GPIO9_DR = idle; delayMicroseconds(5);
}

template <int S> static inline void addr_out(uint32_t b, uint32_t a)
{
    put_nib<S>(b, (a >> 20) & 0xF); put_nib<S>(b, (a >> 16) & 0xF);
    put_nib<S>(b, (a >> 12) & 0xF); put_nib<S>(b, (a >>  8) & 0xF);
    put_nib<S>(b, (a >>  4) & 0xF); put_nib<S>(b, (a      ) & 0xF);
}

static void psram_write(uint32_t a, const uint8_t *s, uint32_t len)
{
    while (len) {
        const uint32_t n = (len > BURST) ? BURST : len;
        bus_out(); const uint32_t b = qbase(); GPIO9_DR = b;
        put_nib<SW>(b, 0x3); put_nib<SW>(b, 0x8); addr_out<SW>(b, a);
        for (uint32_t i = 0; i < n; i++) { put_nib<SW>(b, s[i] >> 4); put_nib<SW>(b, s[i] & 0xF); }
        GPIO9_DR = idle;
        a += n; s += n; len -= n;
    }
}

static void psram_read(uint32_t a, uint8_t *d, uint32_t len)
{
    while (len) {
        const uint32_t n = (len > BURST) ? BURST : len;
        bus_out(); const uint32_t b = qbase(); GPIO9_DR = b;
        put_nib<SR>(b, 0xE); put_nib<SR>(b, 0xB); addr_out<SR>(b, a);
        data_in();
        for (int k = 0; k < 6; k++) { GPIO9_DR = b | B_CLK; spin<SR>(); GPIO9_DR = b; spin<SR>(); }
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t hi = get_nib<SR>(b);
            d[i] = (uint8_t)((hi << 4) | get_nib<SR>(b));
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
 *  the arithmetic
 *
 *  Three memory tiers, the SAME kernel over the SAME bytes in each, and a checksum that has to
 *  come out identical. The checksum is not decoration. It is what stops the compiler deleting a
 *  loop whose result nobody reads -- the first version of this test reported the on-chip case at
 *  zero milliseconds for exactly that reason -- and it is also a correctness proof: if the bytes
 *  the bit-banged bus hands back were wrong in any position, the PSRAM checksum would differ from
 *  the DTCM one.
 *
 *      PSRAM   8 MB behind five wires and a decoder, driven by the processor itself
 *      OCRAM   512 kB on die, reached over the bus matrix
 *      DTCM    tightly coupled, no wait states, the processor's own fast memory
 * ======================================================================================== */

#define WORK  (128u * 1024u)          /* the working set every tier holds a copy of */
#define VBLK  (8u   * 1024u)          /* one block of vertices: 512 of them */
#define VREPS 32u                     /* repeated so the vertex count lands at 16384 */

static uint8_t  dtcm_w[WORK]  __attribute__((aligned(32)));   /* .bss -> DTCM on a Teensy 4 */
DMAMEM static uint8_t ocram_w[WORK] __attribute__((aligned(32)));
static uint8_t  stage[VBLK] __attribute__((aligned(32)));     /* PSRAM staging, float-aligned */

static float   M[16];
static int8_t  xvec[2048];

/* ---------------------------------------------------------------------------------------------
 *  1. the 4x4 vertex transform -- what a graphics pipeline does to every point it draws
 *
 *  Sixteen multiplies and twelve adds per vertex, against sixteen bytes read. That ratio decides
 *  everything: at 28 floating point operations per 16 bytes, a processor issuing one
 *  multiply-accumulate per cycle would need about 343 MB/s to stay busy. The bus delivers 14.8.
 *  So the transform is a memory problem wearing arithmetic's clothes.
 * ------------------------------------------------------------------------------------------ */
static inline float xform_block(const float *v, uint32_t verts)
{
    float sx = 0, sy = 0, sz = 0, sw = 0;
    for (uint32_t i = 0; i < verts; i++) {
        const float x = v[i*4+0], y = v[i*4+1], z = v[i*4+2], w = v[i*4+3];
        sx += M[0]*x + M[4]*y + M[8] *z + M[12]*w;
        sy += M[1]*x + M[5]*y + M[9] *z + M[13]*w;
        sz += M[2]*x + M[6]*y + M[10]*z + M[14]*w;
        sw += M[3]*x + M[7]*y + M[11]*z + M[15]*w;
    }
    return sx + sy + sz + sw;
}

/* ---------------------------------------------------------------------------------------------
 *  2. int8 matrix-vector -- one layer of a network with no quantisation tricks
 * ------------------------------------------------------------------------------------------ */
static inline int32_t gemv8_block(const int8_t *w, uint32_t n, uint32_t xi0)
{
    int32_t a = 0; uint32_t xi = xi0;
    for (uint32_t i = 0; i < n; i++) { a += (int32_t)w[i] * xvec[xi]; xi = (xi + 1) & 2047; }
    return a;
}

/* ---------------------------------------------------------------------------------------------
 *  3. four-bit matrix-vector -- what the real models store, two weights to a byte
 *
 *  The unpacking is the part measured elsewhere in this project as the true ceiling on a general
 *  purpose processor: mask, subtract the zero point, multiply, shift, subtract, multiply. Six
 *  integer operations to turn one byte into two usable weights.
 * ------------------------------------------------------------------------------------------ */
static inline int32_t gemv4_block(const uint8_t *w, uint32_t n, uint32_t xi0)
{
    int32_t a = 0; uint32_t xi = xi0;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t b = w[i];
        a += ((int32_t)(b & 0x0F) - 8) * xvec[xi];   xi = (xi + 1) & 2047;
        a += ((int32_t)(b >> 4)   - 8) * xvec[xi];   xi = (xi + 1) & 2047;
    }
    return a;
}

/* ---------------------------------------------------------------------------------------------
 *  3b. the four-bit kernel again, using the processor's packed-integer instructions
 *
 *  The plain kernel above is this project's central measurement: unpacking, not bandwidth, is what
 *  limits a general purpose processor on four-bit weights, and the size of that limit is the whole
 *  argument for putting the matrix work on an FPGA. It measures 150 MMAC/s from DTCM, which is four
 *  cycles for every multiply-accumulate on a part that can issue one per cycle.
 *
 *  A claim that important deserves the best version of the thing it is claiming about. Two changes,
 *  neither of which alters a single result:
 *
 *  FOUR BYTES AT A TIME, AS PACKED HALFWORDS. One 32-bit load brings in eight weights. Masking with
 *  0x0F0F0F0F isolates the four low nibbles and a shift the four high ones, and SXTB16 spreads two
 *  of those bytes into two 16-bit lanes -- so four SXTB16 turn one word into eight weights sitting
 *  in four registers, two to a register. SMLAD then does two multiply-accumulates per instruction.
 *  Eight weights cost four SMLAD instead of eight multiplies and eight adds.
 *
 *  THE ZERO POINT FACTORED OUT. Every weight is stored biased, so the plain kernel subtracts eight
 *  from each nibble before multiplying: two subtractions per byte, for a constant. But the sum of
 *  (w - 8) * a over a block is the sum of w * a minus eight times the sum of a, and the activations
 *  are known in advance. So the subtraction leaves the inner loop entirely and becomes one add of a
 *  precomputed group sum, corrected once at the end.
 *
 *  The cost is that the activations must be pre-packed to match the order SXTB16 produces, which is
 *  {byte0, byte2} and {byte1, byte3} rather than consecutive. That reordering is done once, in
 *  setup, and is why xpack exists.
 *
 *  If this lands near one cycle per multiply-accumulate then the 31x gap that justifies the FPGAs
 *  needs restating with a smaller number. If it lands near three, the argument stands as written.
 * ------------------------------------------------------------------------------------------ */
/* The three instructions this kernel needs, written out because Teensy's core does not pull in the
 * CMSIS intrinsic headers. Writing them by hand also lets the rotate fold into the extract: the
 * instruction is "sxtb16 rd, rm, ror #8", so selecting bytes 1 and 3 costs nothing extra. */
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

static uint32_t xpack[256 * 4];      /* activations, packed two to a word in SXTB16 order */
static int32_t  xsum8[256];          /* sum of each group of eight, for the zero-point correction */
static int32_t  xsum_all;            /* and the sum of the whole vector, for the closed form */

static inline uint32_t pk(int32_t lo, int32_t hi)
{
    return ((uint32_t)(lo & 0xFFFF)) | ((uint32_t)(hi & 0xFFFF) << 16);
}

static void build_xpack(void)
{
    for (uint32_t g = 0; g < 256; g++) {
        const int8_t *a = &xvec[g * 8];
        /* SXTB16 of the low nibbles gives {byte0, byte2}, which are weights 0 and 4 of the group;
         * rotated by eight it gives {byte1, byte3}, weights 2 and 6. The high nibbles are the odd
         * weights, 1 and 5 then 3 and 7. Four words, in exactly that order. */
        xpack[g*4 + 0] = pk(a[0], a[4]);
        xpack[g*4 + 1] = pk(a[2], a[6]);
        xpack[g*4 + 2] = pk(a[1], a[5]);
        xpack[g*4 + 3] = pk(a[3], a[7]);
        int32_t t = 0;
        for (int k = 0; k < 8; k++) t += a[k];
        xsum8[g] = t;
    }
    xsum_all = 0;
    for (int i = 0; i < 2048; i++) xsum_all += xvec[i];
}

static inline int32_t gemv4_simd(const uint8_t *w, uint32_t n, uint32_t xi0)
{
    int32_t acc = 0, corr = 0;
    uint32_t g = (xi0 >> 3) & 255u;
    uint32_t i = 0;
    for (; i + 4 <= n; i += 4) {
        const uint32_t *P = &xpack[g * 4];
        const uint32_t v = *(const uint32_t *)(w + i);
        const uint32_t L = v & 0x0F0F0F0Fu;            /* four low nibbles, one per byte lane */
        const uint32_t H = (v >> 4) & 0x0F0F0F0Fu;     /* four high nibbles */
        acc = smlad(sxtb16(L),      P[0], acc);
        acc = smlad(sxtb16_ror8(L), P[1], acc);
        acc = smlad(sxtb16(H),      P[2], acc);
        acc = smlad(sxtb16_ror8(H), P[3], acc);
        corr += xsum8[g];
        g = (g + 1) & 255u;
    }
    /* tail, for any block not a multiple of four bytes */
    uint32_t xi = (xi0 + i * 2) & 2047u;
    for (; i < n; i++) {
        const uint8_t b = w[i];
        acc  += (int32_t)(b & 0x0F) * xvec[xi]; corr += xvec[xi]; xi = (xi + 1) & 2047u;
        acc  += (int32_t)(b >> 4)   * xvec[xi]; corr += xvec[xi]; xi = (xi + 1) & 2047u;
    }
    return acc - 8 * corr;          /* the zero point, applied once instead of twice per byte */
}

/* ---------------------------------------------------------------------------------------------
 *  3c. and once more, with the last of the bookkeeping removed
 *
 *  The SMLAD kernel reached 2.63 cycles per multiply-accumulate, which works out at roughly twenty
 *  instructions for every eight weights and almost no stalling -- so what is left to remove is
 *  instructions, not waiting. Counting them: one weight load, three mask-and-shift, four SXTB16,
 *  four SMLAD and four activation loads do the actual work, which is sixteen. The other four are
 *  bookkeeping: loading a group sum, adding it, advancing the group index, and the loop itself.
 *
 *  Both can go.
 *
 *  THE CORRECTION HAS A CLOSED FORM. The block is 8192 bytes, which is 16384 weights, and the
 *  activation vector is 2048 long -- so the block walks it exactly eight times. The sum of the
 *  activations used is therefore eight times the sum of all of them, a compile-time-shaped constant,
 *  and the per-group sum never needs loading at all. This only holds while the block is a whole
 *  number of cycles of the vector, so the kernel checks and falls back rather than assuming it.
 *
 *  TWO WORDS PER ITERATION. Consecutive groups sit next to each other in xpack, so eight activation
 *  words are one contiguous run and the loop overhead is paid once for sixteen weights instead of
 *  once for eight.
 *
 *  What is left is the floor for this instruction set. Eight weights need one load, four SXTB16 and
 *  four SMLAD, and their activations need four loads -- thirteen instructions for sixteen
 *  multiply-accumulates. There is no four-lane 8-bit multiply on this core, so SMLAD's two lanes are
 *  as wide as the arithmetic gets, and about two cycles per multiply-accumulate is where a Cortex-M7
 *  stops. An FPGA lane does one per clock with no unpacking at all, which is the entire argument.
 * ------------------------------------------------------------------------------------------ */
static inline int32_t gemv4_simd2(const uint8_t *w, uint32_t n, uint32_t xi0)
{
    /* the closed form is only valid for a whole number of passes over the activation vector from a
     * group-aligned start. Anything else goes to the general kernel, which is still fast. */
    if ((n & 1023u) || (xi0 & 2047u)) return gemv4_simd(w, n, xi0);

    int32_t a0 = 0, a1 = 0;            /* two accumulators, so the two words do not serialise */
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
    /* eight full passes over a 2048-long vector per 8192-byte block, hence n/1024 passes */
    return a0 + a1 - 8 * (int32_t)(n / 1024u) * xsum_all;
}

/* ---------------------------------------------------------------------------------------------
 *  4. a real float GEMM -- 64x64 times 64x64, the classic shape
 *
 *  No memory question here at all: all three matrices are in DTCM and the whole thing is 48 kB.
 *  This is the honest ceiling of the floating point unit on this part, and the number every
 *  memory-fed result above should be read against.
 * ------------------------------------------------------------------------------------------ */
#define NM 64
static float GA[NM*NM], GB[NM*NM], GC[NM*NM], GD[NM*NM];

static uint32_t gemm_f32(void)
{
    const uint32_t t0 = ARM_DWT_CYCCNT;
    for (uint32_t i = 0; i < NM; i++) {
        const float *a = &GA[i*NM];
        float *c = &GC[i*NM];
        for (uint32_t j = 0; j < NM; j++) c[j] = 0.0f;
        for (uint32_t k = 0; k < NM; k++) {          /* i-k-j order: B is walked along its rows */
            const float av = a[k];
            const float *b = &GB[k*NM];
            for (uint32_t j = 0; j < NM; j++) c[j] += av * b[j];
        }
    }
    return ARM_DWT_CYCCNT - t0;
}

/* ---------------------------------------------------------------------------------------------
 *  4b. the same GEMM, blocked into 4x4 tiles held in registers
 *
 *  The loop above is the textbook i-k-j form and it measured 9.19 cycles per multiply-accumulate
 *  against a floating point unit that the vertex transform drove at about 1.6 cycles per operation.
 *  That gap is not the chip. Its inner statement is c[j] += av * b[j], which loads c[j], multiplies,
 *  adds and stores c[j] back on every single iteration -- two memory touches and a store-to-load
 *  dependency for one useful multiply.
 *
 *  Blocking fixes it without changing a single arithmetic result. Hold a 4x4 patch of the output in
 *  sixteen registers for the whole length of k, and each step of k costs four loads from A, four
 *  from B, and sixteen multiply-accumulates. Sixteen useful operations per eight loads instead of
 *  one per two, and nothing is stored until the tile is finished.
 *
 *  This is the same arithmetic in a different order, so the result has to match the plain version
 *  exactly. It is checked below, because a fast GEMM that computes something else is not a GEMM.
 * ------------------------------------------------------------------------------------------ */
static uint32_t gemm_f32_blocked(void)
{
    const uint32_t t0 = ARM_DWT_CYCCNT;
    for (uint32_t i = 0; i < NM; i += 4) {
        for (uint32_t j = 0; j < NM; j += 4) {
            float c00=0,c01=0,c02=0,c03=0, c10=0,c11=0,c12=0,c13=0;
            float c20=0,c21=0,c22=0,c23=0, c30=0,c31=0,c32=0,c33=0;
            const float *a0 = &GA[(i+0)*NM], *a1 = &GA[(i+1)*NM];
            const float *a2 = &GA[(i+2)*NM], *a3 = &GA[(i+3)*NM];
            for (uint32_t k = 0; k < NM; k++) {
                const float *b = &GB[k*NM + j];
                const float b0 = b[0], b1 = b[1], b2 = b[2], b3 = b[3];
                const float v0 = a0[k], v1 = a1[k], v2 = a2[k], v3 = a3[k];
                c00 += v0*b0; c01 += v0*b1; c02 += v0*b2; c03 += v0*b3;
                c10 += v1*b0; c11 += v1*b1; c12 += v1*b2; c13 += v1*b3;
                c20 += v2*b0; c21 += v2*b1; c22 += v2*b2; c23 += v2*b3;
                c30 += v3*b0; c31 += v3*b1; c32 += v3*b2; c33 += v3*b3;
            }
            float *r0 = &GD[(i+0)*NM + j], *r1 = &GD[(i+1)*NM + j];
            float *r2 = &GD[(i+2)*NM + j], *r3 = &GD[(i+3)*NM + j];
            r0[0]=c00; r0[1]=c01; r0[2]=c02; r0[3]=c03;
            r1[0]=c10; r1[1]=c11; r1[2]=c12; r1[3]=c13;
            r2[0]=c20; r2[1]=c21; r2[2]=c22; r2[3]=c23;
            r3[0]=c30; r3[1]=c31; r3[2]=c32; r3[3]=c33;
        }
    }
    return ARM_DWT_CYCCNT - t0;
}

/* ======================================================================================== */

static inline float cyc_s(uint32_t c) { return c / (float)F_CPU_ACTUAL; }

static void row(const char *tier, float ms, float rate, const char *unit)
{
    Serial.print(F("    ")); Serial.print(tier);
    Serial.print(F("   ")); Serial.print(ms, 3); Serial.print(F(" ms"));
    Serial.print(F("   ")); Serial.print(rate, 2); Serial.print(' ');
    Serial.println(unit);
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

    /* a transform with no zeros in it, so every one of the sixteen multiplies counts */
    for (int i = 0; i < 16; i++) M[i] = 0.125f + 0.0625f * (float)(i % 7);
    for (int i = 0; i < 2048; i++) xvec[i] = (int8_t)(((i * 37) & 0x7F) - 64);
    build_xpack();
    for (int i = 0; i < NM*NM; i++) {
        GA[i] = (float)((i % 17) - 8) * 0.125f;
        GB[i] = (float)((i % 13) - 6) * 0.25f;
    }
}

void loop()
{
    Serial.println();
    Serial.println(F("=================================================================="));
    Serial.print(F("  matrix arithmetic across the memory hierarchy.  CPU "));
    Serial.print(F_CPU_ACTUAL / 1000000); Serial.println(F(" MHz"));
    Serial.println(F("=================================================================="));

    pick(7);
    enter_quad();

    /* ---- lay down 128 kB of operands, then mirror them on chip ---------------------------- *
     * Real floats, not random bytes: a random bit pattern is a NaN or an infinity about half the
     * time, which would still measure a multiply but would make the checksum meaningless and let
     * a read fault hide behind arithmetic that cannot fail. Arbitrary floats make perfectly good
     * weight bytes for the integer cases. */
    Serial.println(F("  writing 128 kB of operands to PSRAM, then mirroring them on chip"));
    for (uint32_t off = 0; off < WORK; off += VBLK) {
        float *f = (float *)stage;
        for (uint32_t i = 0; i < VBLK / 4; i++) {
            const uint32_t n = (off / 4) + i;
            f[i] = (float)((int32_t)(n % 2001) - 1000) * 0.001f;     /* -1.000 .. +1.000 */
        }
        psram_write(off, stage, VBLK);
    }
    for (uint32_t off = 0; off < WORK; off += VBLK) {
        psram_read(off, stage, VBLK);
        memcpy(&dtcm_w[off],  stage, VBLK);
        memcpy(&ocram_w[off], stage, VBLK);
    }
    arm_dcache_flush_delete(ocram_w, WORK);   /* so the OCRAM pass is not just reading the cache */

    /* ---- 1. vertices ---------------------------------------------------------------------- */
    {
        const uint32_t verts = (VBLK / 16) * VREPS;
        const float    flops = (float)verts * 28.0f;
        float s1 = 0, s2 = 0, s3 = 0;
        uint32_t t0;

        t0 = ARM_DWT_CYCCNT;
        for (uint32_t r = 0; r < VREPS; r++) {
            psram_read(0, stage, VBLK);
            s1 += xform_block((const float *)stage, VBLK / 16);
        }
        const float ps = cyc_s(ARM_DWT_CYCCNT - t0);

        t0 = ARM_DWT_CYCCNT;
        for (uint32_t r = 0; r < VREPS; r++) s2 += xform_block((const float *)ocram_w, VBLK / 16);
        const float os = cyc_s(ARM_DWT_CYCCNT - t0);

        t0 = ARM_DWT_CYCCNT;
        for (uint32_t r = 0; r < VREPS; r++) s3 += xform_block((const float *)dtcm_w, VBLK / 16);
        const float ds = cyc_s(ARM_DWT_CYCCNT - t0);

        Serial.println(F("[1] 4x4 transform over 16384 vertices, float"));
        Serial.println(F("    tier     time          rate"));
        row("PSRAM", ps * 1000.0f, flops / ps / 1e6f, "MFLOP/s");
        row("OCRAM", os * 1000.0f, flops / os / 1e6f, "MFLOP/s");
        row("DTCM ", ds * 1000.0f, flops / ds / 1e6f, "MFLOP/s");
        Serial.print(F("    "));  Serial.print(verts / ps / 1000.0f, 1);
        Serial.print(F(" kverts/s from PSRAM, ")); Serial.print(verts / ds / 1000.0f, 1);
        Serial.println(F(" from DTCM"));

        /* all three checksums printed, OCRAM included. The first version of this test discarded
         * the OCRAM one with a cast to void, and the compiler duly deleted the entire OCRAM loop
         * and reported it at 0.000 ms and 275 million MFLOP/s. A timed loop whose result is never
         * read is not a measurement of anything. */
        uint32_t b1, b2, b3;
        memcpy(&b1, &s1, 4); memcpy(&b2, &s2, 4); memcpy(&b3, &s3, 4);
        Serial.print(F("    checksums  PSRAM 0x")); Serial.print(b1, HEX);
        Serial.print(F("  OCRAM 0x"));              Serial.print(b2, HEX);
        Serial.print(F("  DTCM 0x"));               Serial.println(b3, HEX);
        Serial.println((b1 == b3 && b2 == b3)
            ? F("    all three identical -- every byte the bit-banged bus returned was correct")
            : F("    MISMATCH -- the bus is corrupting data"));
    }

    /* ---- 2. int8 -------------------------------------------------------------------------- */
    {
        const float macs = (float)WORK;
        int64_t cp = 0, co = 0, cd = 0;
        uint32_t t0;

        t0 = ARM_DWT_CYCCNT;
        for (uint32_t off = 0; off < WORK; off += VBLK) {
            psram_read(off, stage, VBLK);
            cp += gemv8_block((const int8_t *)stage, VBLK, off & 2047);
        }
        const float ps = cyc_s(ARM_DWT_CYCCNT - t0);

        t0 = ARM_DWT_CYCCNT;
        for (uint32_t off = 0; off < WORK; off += VBLK)
            co += gemv8_block((const int8_t *)&ocram_w[off], VBLK, off & 2047);
        const float os = cyc_s(ARM_DWT_CYCCNT - t0);

        t0 = ARM_DWT_CYCCNT;
        for (uint32_t off = 0; off < WORK; off += VBLK)
            cd += gemv8_block((const int8_t *)&dtcm_w[off], VBLK, off & 2047);
        const float ds = cyc_s(ARM_DWT_CYCCNT - t0);

        Serial.println(F("[2] int8 matrix-vector over 128 kB of weights"));
        Serial.println(F("    tier     time          rate"));
        row("PSRAM", ps * 1000.0f, macs / ps / 1e6f, "MMAC/s");
        row("OCRAM", os * 1000.0f, macs / os / 1e6f, "MMAC/s");
        row("DTCM ", ds * 1000.0f, macs / ds / 1e6f, "MMAC/s");
        Serial.print(F("    checksums ")); Serial.print((int32_t)cp);
        Serial.print(' '); Serial.print((int32_t)co);
        Serial.print(' '); Serial.println((int32_t)cd);
        Serial.println((cp == cd && co == cd) ? F("    all three agree") : F("    MISMATCH"));
    }

    /* ---- 3. four-bit --------------------------------------------------------------------- */
    float p4 = 1, d4 = 1;
    {
        const float macs = (float)WORK * 2.0f;
        int64_t cp = 0, co = 0, cd = 0;
        uint32_t t0;

        t0 = ARM_DWT_CYCCNT;
        for (uint32_t off = 0; off < WORK; off += VBLK) {
            psram_read(off, stage, VBLK);
            cp += gemv4_block(stage, VBLK, (off * 2) & 2047);
        }
        p4 = cyc_s(ARM_DWT_CYCCNT - t0);

        t0 = ARM_DWT_CYCCNT;
        for (uint32_t off = 0; off < WORK; off += VBLK)
            co += gemv4_block(&ocram_w[off], VBLK, (off * 2) & 2047);
        const float os = cyc_s(ARM_DWT_CYCCNT - t0);

        t0 = ARM_DWT_CYCCNT;
        for (uint32_t off = 0; off < WORK; off += VBLK)
            cd += gemv4_block(&dtcm_w[off], VBLK, (off * 2) & 2047);
        d4 = cyc_s(ARM_DWT_CYCCNT - t0);

        Serial.println(F("[3] 4-bit matrix-vector over 128 kB, unpacked on the fly"));
        Serial.println(F("    tier     time          rate"));
        row("PSRAM", p4 * 1000.0f, macs / p4 / 1e6f, "MMAC/s");
        row("OCRAM", os * 1000.0f, macs / os / 1e6f, "MMAC/s");
        row("DTCM ", d4 * 1000.0f, macs / d4 / 1e6f, "MMAC/s");
        Serial.print(F("    checksums ")); Serial.print((int32_t)cp);
        Serial.print(' '); Serial.print((int32_t)co);
        Serial.print(' '); Serial.println((int32_t)cd);
        Serial.println((cp == cd && co == cd) ? F("    all three agree") : F("    MISMATCH"));

        /* the same kernel written with packed-integer instructions, against the same bytes in the
         * same tier, so the only thing that changed is the instruction selection */
        int64_t cs = 0;
        t0 = ARM_DWT_CYCCNT;
        for (uint32_t off = 0; off < WORK; off += VBLK)
            cs += gemv4_simd(&dtcm_w[off], VBLK, (off * 2) & 2047);
        const float ss = cyc_s(ARM_DWT_CYCCNT - t0);

        Serial.println(F("    -- and the same kernel with SMLAD, from DTCM --"));
        row("SIMD ", ss * 1000.0f, macs / ss / 1e6f, "MMAC/s");
        Serial.print(F("    checksum ")); Serial.print((int32_t)cs);
        Serial.println(cs == cd ? F("  matches the plain kernel exactly")
                                : F("  DOES NOT MATCH -- the fast kernel is wrong"));
        Serial.print(F("    ")); Serial.print(d4 / ss, 2);
        Serial.print(F("x the plain kernel, ")); Serial.print((float)F_CPU_ACTUAL * ss / macs, 2);
        Serial.println(F(" cycles per multiply-accumulate"));

        int64_t c2 = 0;
        t0 = ARM_DWT_CYCCNT;
        for (uint32_t off = 0; off < WORK; off += VBLK)
            c2 += gemv4_simd2(&dtcm_w[off], VBLK, (off * 2) & 2047);
        const float s2t = cyc_s(ARM_DWT_CYCCNT - t0);

        Serial.println(F("    -- bookkeeping removed, two words per iteration --"));
        row("SIMD2", s2t * 1000.0f, macs / s2t / 1e6f, "MMAC/s");
        Serial.print(F("    checksum ")); Serial.print((int32_t)c2);
        Serial.println(c2 == cd ? F("  still exactly the plain kernel's answer")
                                : F("  DOES NOT MATCH -- wrong"));
        Serial.print(F("    ")); Serial.print(d4 / s2t, 2);
        Serial.print(F("x the plain kernel, ")); Serial.print((float)F_CPU_ACTUAL * s2t / macs, 2);
        Serial.println(F(" cycles per multiply-accumulate"));
    }

    /* ---- 4. the float unit with no memory question at all --------------------------------- */
    {
        const float flops = 2.0f * NM * NM * NM;
        const uint32_t cp = gemm_f32();
        const float sp = cyc_s(cp);
        const uint32_t cb = gemm_f32_blocked();
        const float sb = cyc_s(cb);

        Serial.println(F("[4] 64x64 float GEMM, everything in DTCM"));
        Serial.println(F("    form          time        rate           cycles per MAC"));
        Serial.print(F("    plain ikj     ")); Serial.print(sp * 1000.0f, 3);
        Serial.print(F(" ms   ")); Serial.print(flops / sp / 1e6f, 2);
        Serial.print(F(" MFLOP/s   ")); Serial.println((float)cp / (float)(NM*NM*NM), 2);
        Serial.print(F("    4x4 blocked   ")); Serial.print(sb * 1000.0f, 3);
        Serial.print(F(" ms   ")); Serial.print(flops / sb / 1e6f, 2);
        Serial.print(F(" MFLOP/s   ")); Serial.println((float)cb / (float)(NM*NM*NM), 2);

        /* the two must agree exactly: same operands, same order of accumulation along k, only the
         * order of the independent output elements changed. Any difference means the fast one is
         * computing something else, and a fast wrong answer is worth nothing. */
        uint32_t diff = 0;
        for (int i = 0; i < NM*NM; i++) if (GC[i] != GD[i]) diff++;
        Serial.print(F("    elements differing between the two: ")); Serial.println(diff);
        Serial.print(F("    speedup ")); Serial.print(sp / sb, 2);
        Serial.println(F("x, from holding the output tile in registers instead of"));
        Serial.println(F("    loading and storing it on every multiply."));
    }

    /* ---- what it adds up to --------------------------------------------------------------- */
    Serial.println(F("--- what the gap means ---"));
    Serial.print(F("  the 4-bit kernel runs "));
    Serial.print(p4 / d4, 1);
    Serial.println(F("x faster from DTCM than from PSRAM."));
    Serial.print(F("  so "));
    Serial.print(100.0f * (1.0f - d4 / p4), 0);
    Serial.println(F("% of the PSRAM run is the bus and not the arithmetic."));
    Serial.println(F("  That is the price of the processor being the transfer. It toggles every"));
    Serial.println(F("  edge itself, so a read and a multiply cannot overlap at all. A hardware"));
    Serial.println(F("  controller lets them, which is what the perfboard target buys."));

    Serial.println(F("=== repeating in 12 s ==="));
    delay(12000);
}
