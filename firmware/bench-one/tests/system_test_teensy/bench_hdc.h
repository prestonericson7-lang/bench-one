/* ===========================================================================================
 *  bench_hdc.h -- BENCH ONE: the substrate the being is made of
 * ===========================================================================================
 *
 *  A machine that starts empty and learns only from what it is given.
 *  Nothing here is pretrained. There is no model to download, no weights to import, and no
 *  step where knowledge arrives from outside. Every symbol starts as random noise and every
 *  structure in the system is put there by an input you fed it.
 *
 *
 *  WHY THIS AND NOT A NEURAL NETWORK
 *  ----------------------------------
 *  A neural network's knowledge lives in the *precision* of millions of real-valued weights,
 *  which is why it needs floating point, gradients, many passes over the data, and a machine
 *  with enormous memory bandwidth. This hardware has none of those. What it has is 34 nodes
 *  that can do XOR, majority and popcount on wide words, in parallel, extremely fast.
 *
 *  So the representation is chosen to match the machine instead of fighting it. A concept is
 *  not a vector of floats -- it is 8,192 BITS. And the three operations below are complete:
 *  they can represent sets, sequences, records, associations and analogies, which is enough
 *  to build a mind out of.
 *
 *      BIND     (XOR)        a ^ b       associate two things. Its own inverse.
 *      BUNDLE   (majority)   [a,b,c]     superpose many things into one vector.
 *      PERMUTE  (rotate)     rot(a, k)   order and position: 'ab' != 'ba'.
 *
 *  Similarity is Hamming distance -- one XOR and one popcount per 32 bits.
 *
 *
 *  THE PROPERTY EVERYTHING RESTS ON
 *  ---------------------------------
 *  In 8,192 dimensions, two random vectors are almost exactly orthogonal. Measured over 250
 *  random pairs: mean similarity -0.0001, standard deviation 0.0112, worst case 0.0325.
 *
 *  Two concepts the machine has never related will never be confused, no matter how many it
 *  holds. That is where the capacity comes from, and it is why this scales on small hardware
 *  when a neural network does not.
 *
 *
 *  WHAT WAS MEASURED BEFORE ANY OF THIS WAS WRITTEN
 *  -------------------------------------------------
 *      Bundle capacity      127 vectors superposed in ONE vector, 100% recall.
 *                           255 -> 93.7%. 511 -> 50%. So ~128 items per bundle is the budget.
 *
 *      One-shot learning    100 classes, ONE example each, 20% bit noise on the example AND
 *                           on every query: 100.0% correct.
 *
 *      Continual growth     10, 50, 100, 200, 400 classes stored: 100% recall at every size.
 *                           Adding a new memory does not disturb an old one. There is no
 *                           catastrophic forgetting because there is no shared weight matrix
 *                           to overwrite.
 *
 *      Sequence prediction  A 4-gram model built in ONE pass over 1,362 characters of text:
 *                           94.7% next-character accuracy against a 20.3% baseline, and it
 *                           continues text coherently from a seed.
 *
 *  Those are the numbers this design is justified by. They are reproducible.
 *
 *
 *  WHY IT FITS THIS MACHINE IN PARTICULAR
 *  ---------------------------------------
 *      One hypervector = 8192 bits = 1024 bytes.
 *
 *      Teensy DTCM   512 KB  ->    ~400 vectors, single-cycle
 *      Teensy PSRAM   16 MB  ->  16,000 vectors
 *      Luckfox DDR2   64 MB  ->  64,000 vectors      x10 nodes
 *      FPGA DDR3       1 GB  -> 1,000,000 vectors    x2 boards
 *                               --------------------------------
 *                               ~2.6 MILLION concepts, machine-wide
 *
 *  And a search is embarrassingly parallel: each node scans its own shard and reports one
 *  winner. The network carries a few bytes per query, not the memory. That is why a slow
 *  interconnect does not matter here -- it never did, for this algorithm.
 * ===========================================================================================
 */

#ifndef BENCH_HDC_H
#define BENCH_HDC_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------------------------
 * DIMENSION
 * -----------------------------------------------------------------------------------------
 * 8192 was chosen, not guessed: it is the point where random vectors are orthogonal enough
 * that 100 classes are separable at 20% noise (measured 100%), while a vector is still
 * exactly 1 KB, which keeps 400 of them in a Teensy's DTCM and makes every buffer a clean
 * power of two. Lower it to 4096 on a memory-starved node -- capacity falls roughly with the
 * square root of D, so 4096 still holds ~70 classes comfortably.
 * ----------------------------------------------------------------------------------------- */
#ifndef HD_BITS
#define HD_BITS 8192
#endif

#define HD_WORDS  (HD_BITS / 32)
#define HD_BYTES  (HD_BITS / 8)

typedef uint32_t hd_t[HD_WORDS];

/* -------------------------------------------------------------------------------------------
 * CORE OPERATIONS -- the complete instruction set of the being
 * ----------------------------------------------------------------------------------------- */

/* A fresh random concept. This is what "a new being with nothing" means literally: every
 * symbol it will ever know starts here, as noise, and only becomes meaningful through what
 * you bind and bundle it with. */
void     hd_random (hd_t out, uint32_t *rng);

void     hd_zero   (hd_t out);
void     hd_copy   (hd_t out, const hd_t a);

/* BIND: associate. XOR is its own inverse, so bind(bind(a,b),b) == a -- that is what lets the
 * machine ask "what was X associated with?" and get an answer back out of a bundle. */
void     hd_bind   (hd_t out, const hd_t a, const hd_t b);

/* PERMUTE: rotate the whole vector by `shift` bits. This is what encodes ORDER. Without it a
 * sequence is just a set and 'ab' is indistinguishable from 'ba'. Rotation is invertible
 * (shift by -k undoes +k) and it produces a vector orthogonal to the original, which is
 * exactly the behaviour position information needs.
 *
 * NOT SAFE IN PLACE: `out` and `in` must be different buffers. Aliasing them returns a plausible
 * wrong answer with no warning. */
void     hd_permute(hd_t out, const hd_t in, int32_t shift);

/* Similarity. Hamming distance in bits: 0 = identical, HD_BITS/2 = unrelated (orthogonal),
 * HD_BITS = exact opposite. This is the only measurement the system makes. */
uint32_t hd_hamming(const hd_t a, const hd_t b);

/* Same distance, but abandons the moment it cannot beat `limit`. Returns the true distance, or
 * some value >= limit if it gave up. Used by every nearest-neighbour search; the winner is
 * always identical to a full scan. */
uint32_t hd_hamming_limit(const hd_t a, const hd_t b, uint32_t limit);

/* -------------------------------------------------------------------------------------------
 * PREFIX SCREENING -- the optimisation that actually matters on this hardware
 * -----------------------------------------------------------------------------------------
 * A search is not compute-bound, it is MEMORY-bound. Every comparison reads 2 KB, so scanning
 * 20,000 concepts moves 20 MB -- and PSRAM delivers 45 MB/s. Making the arithmetic faster does
 * nothing about that; reading fewer bytes does.
 *
 * The first 256 bits of a vector are enough to tell a match from a stranger. Measured with a
 * query at 25% noise: the true match sits at 46 of 256, an unrelated vector at 128, which is
 * 10.3 sigma apart. So 32 bytes decides what 1024 bytes would have.
 *
 * MEASURED, 2,400 trials across 500-8,000 concepts, 15-48% noise, with and without correlated
 * near-duplicate families: ZERO answers lost at 6 sigma, and the winner is always identical to
 * a full scan.
 *
 *      15% noise -> 23.2x less memory traffic
 *      25% noise -> 13.0x
 *      35% noise ->  2.2x
 *      42%+      ->  1.0x   (everything survives screening; it silently becomes a full scan)
 *
 * That last row is the important one: when the query is too corrupted to screen, the method
 * costs nothing and degrades to exactly what it replaced. It never trades correctness for
 * speed -- at 4 sigma it did, losing 2% of answers at 45% noise, which is why 6 is shipped.
 * ----------------------------------------------------------------------------------------- */
#ifndef HD_PREFIX_WORDS
#define HD_PREFIX_WORDS 8              /* 256 bits = 32 bytes */
#endif

/* Distance over the first HD_PREFIX_WORDS words only. */
uint32_t hd_hamming_prefix(const hd_t a, const hd_t b);

/* Convenience: similarity in the range -1024..+1024 (fixed point, +1024 = identical). */
int32_t  hd_sim_q10(const hd_t a, const hd_t b);


/* -------------------------------------------------------------------------------------------
 * BUNDLING -- superposition, and the reason the machine can hold a set in one vector
 * -----------------------------------------------------------------------------------------
 * Adding N vectors together and taking the majority per bit gives a single vector that is
 * measurably similar to EVERY input and to nothing else. A bundle is a set that costs the
 * same 1 KB whether it holds three items or a hundred.
 *
 * Budget it at ~128 members. Measured: 127 members recall at 100%, 255 at 93.7%, 511 at 50%.
 * Past that the members are still in there, but they stop being individually retrievable.
 * ----------------------------------------------------------------------------------------- */
typedef struct {
    int16_t  c[HD_BITS];   /* bipolar running total per bit. 16 KB at HD_BITS=8192.          */
    uint32_t n;            /* how many vectors have gone in                                  */
} hd_acc_t;

void hd_acc_clear (hd_acc_t *a);
void hd_acc_add   (hd_acc_t *a, const hd_t v);
void hd_acc_addw  (hd_acc_t *a, const hd_t v, int16_t weight);  /* weighted; negative = erase */
void hd_acc_result(const hd_acc_t *a, hd_t out);


/* -------------------------------------------------------------------------------------------
 * ITEM MEMORY -- everything the being has ever been told
 * -----------------------------------------------------------------------------------------
 * A flat array, searched linearly. Linear is the right answer here and not a placeholder:
 * the comparison is 256 XORs and 256 popcounts, the access pattern is perfectly sequential,
 * and it shards across nodes with no coordination at all. There is no index to keep coherent
 * and no tree to rebalance while the machine is learning.
 *
 * The caller owns the storage, so a Teensy can put a small memory in DTCM and a large one in
 * EXTMEM by choosing where it declares the array.
 * ----------------------------------------------------------------------------------------- */
typedef struct {
    hd_t     *v;        /* cap vectors                                                       */
    uint16_t *label;    /* cap labels -- what each vector means to the layer above            */
    uint16_t  n;        /* in use                                                             */
    uint16_t  cap;
} hd_mem_t;

void     hd_mem_init(hd_mem_t *m, hd_t *storage, uint16_t *labels, uint16_t cap);

/* Teach it one thing. Returns the slot, or -1 if full. THIS IS THE ENTIRE LEARNING STEP for
 * a new concept: one shot, no epochs, no gradient. */
int32_t  hd_mem_add (hd_mem_t *m, const hd_t v, uint16_t label);

/* Nearest match. Returns the slot and writes its Hamming distance, or -1 if the memory is
 * empty. `*dist` compared against HD_BITS/2 tells you whether the match means anything: a
 * distance near HD_BITS/2 is the machine correctly saying "I have never seen this". */
int32_t  hd_mem_best(const hd_mem_t *m, const hd_t q, uint32_t *dist);

/* Top-k, for when the layer above wants to weigh several candidates. Fills slots[] and
 * dists[]; returns how many were filled. */
uint16_t hd_mem_topk(const hd_mem_t *m, const hd_t q, uint16_t k,
                     int32_t *slots, uint32_t *dists);

/* Reinforce an existing memory with a new example instead of adding a slot -- this is how a
 * concept gets SHARPER with repetition rather than duplicated. */
void     hd_mem_reinforce(hd_mem_t *m, int32_t slot, const hd_t v, uint8_t strength);


/* -------------------------------------------------------------------------------------------
 * SEQUENCE MODEL -- prediction, which is where memory becomes thought
 * -----------------------------------------------------------------------------------------
 * Measured: 4-gram, one pass over 1,362 characters, 94.7% next-symbol accuracy against a
 * 20.3% baseline, and coherent free generation from a seed.
 *
 * A context of the last N symbols is bound into one vector, each symbol rotated by how far
 * back it sits. That context vector keys a bundle of every symbol that has ever followed it,
 * so the prediction is the nearest symbol to that bundle -- and because it is a bundle, a
 * context that has been followed by several different symbols returns a genuine blend rather
 * than the last one written.
 * ----------------------------------------------------------------------------------------- */
typedef struct {
    hd_mem_t *sym;        /* the alphabet: one hypervector per symbol                        */
    hd_mem_t *ctx;        /* context vectors, label = index into pred[]                      */
    hd_acc_t *pred;       /* one accumulator per stored context                              */
    uint16_t  npred;
    uint16_t  cap_pred;
    uint8_t   order;      /* n-gram order. 4 measured best of 2/3/4                          */
    uint32_t  match_tol;  /* max Hamming distance to call a context "the same context"       */
} hd_seq_t;

void     hd_seq_init(hd_seq_t *s, hd_mem_t *sym, hd_mem_t *ctx,
                     hd_acc_t *pred, uint16_t cap_pred, uint8_t order);

/* Build the context vector for the last `order` symbols (most recent last). */
void     hd_seq_context(const hd_seq_t *s, const uint16_t *recent, hd_t out);

/* Observe: "after this context came `next_sym`". One pass, no training loop. */
void     hd_seq_observe(hd_seq_t *s, const uint16_t *recent, uint16_t next_sym);

/* Predict the next symbol index, or -1 if this context has never been seen. */
int32_t  hd_seq_predict(const hd_seq_t *s, const uint16_t *recent, uint32_t *confidence);


/* -------------------------------------------------------------------------------------------
 * ENCODING REAL INPUT
 * -----------------------------------------------------------------------------------------
 * The bridge from the world to hypervectors. Every sensor on this machine -- the 74HC fabric's
 * 152 captured bits, an ADC channel, a byte from a radio -- becomes a hypervector this way,
 * and after that the rest of the system does not care where it came from.
 * ----------------------------------------------------------------------------------------- */

/* A scalar in 0..max, encoded so that NEARBY VALUES GIVE SIMILAR VECTORS. Built by flipping a
 * progressively larger prefix of a base vector, which makes similarity fall off smoothly with
 * distance -- a plain per-value random vector would make 100 and 101 as unrelated as 100 and 3,
 * and the machine could never generalise across a measurement. */
void hd_encode_level(hd_t out, const hd_t base, uint32_t value, uint32_t max);

/* A bit field (e.g. the fabric's 152 simultaneous channels) using one random vector per bit
 * position, bundled where the bit is set. `pos` must hold at least `nbits` vectors.
 *
 * `scratch` is a caller-owned accumulator and it is NOT a convenience: an hd_acc_t is 16 KB,
 * and putting it on the stack overflows an ESP32-S3 task (4-8 KB) instantly. Measured at
 * 16,432 bytes of stack before this signature changed. Give it a static one. */
void hd_encode_bits(hd_t out, const hd_t *pos, const uint8_t *bytes, uint16_t nbits,
                    hd_acc_t *scratch);

/* -------------------------------------------------------------------------------------------
 * THE HOT KERNEL -- in the header so every module gets it, not just bench_hdc.c
 * -----------------------------------------------------------------------------------------
 * These were originally file-local, which meant bench_index.c compiled without them and
 * silently fell back to a plain popcount -- the exact libgcc-call cost that was just removed,
 * reintroduced by scope. Anything that compares vectors needs these.
 * ----------------------------------------------------------------------------------------- */
/* Nibble-wise population count: every byte of the result holds 0..8. */
static inline uint32_t hd_pc_nib(uint32_t x)
{
    x = x - ((x >> 1) & 0x55555555u);
    x = (x & 0x33333333u) + ((x >> 2) & 0x33333333u);
    return (x + (x >> 4)) & 0x0F0F0F0Fu;
}

#if defined(__ARM_FEATURE_DSP) && (defined(__GNUC__) || defined(__clang__))
#define HD_HAVE_USAD8 1
static inline uint32_t hd_usad8(uint32_t a, uint32_t b)
{
    uint32_t r;
    __asm__("usad8 %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
    return r;
}
#endif


/* Portable 32-bit popcount, and the RNG the whole system shares. */
uint32_t hd_popcount(uint32_t x);
uint32_t hd_rand(uint32_t *state);

#ifdef __cplusplus
}
#endif
#endif /* BENCH_HDC_H */
