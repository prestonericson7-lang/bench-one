/* ===========================================================================================
 *  bench_dbn.h -- BENCH ONE: one node = one layer of the deep belief network
 * ===========================================================================================
 *
 *  This is the file that turns 34 boards into one machine.
 *
 *
 *  WHY A DBN AND NOT BACKPROP -- THE REASON THE WHOLE ARCHITECTURE WORKS
 *  ---------------------------------------------------------------------
 *  A deep belief network is trained greedily, one layer at a time. Layer k is trained to model
 *  the distribution of whatever layer k-1 produces, and while it trains it needs NOTHING from
 *  any other layer -- no gradients flowing back, no synchronised parameter server, no
 *  all-reduce. When it has converged it is frozen, and from then on it only ever pushes its
 *  hidden activations downstream.
 *
 *  So the links between nodes carry ACTIVATIONS, never weights and never gradients:
 *
 *      32 replicas x 500 hidden units, bit-packed  =  2,048 bytes per batch
 *          over 100 Mbit Ethernet   ~0.2 ms
 *          over 921600 baud UART    ~22 ms
 *          over WiFi                ~1 ms
 *
 *  against a batch that takes tens of milliseconds to train. THAT is why this machine can be
 *  built out of slow links and cheap boards. Backprop across 34 nodes would need the gradient
 *  of every weight moved every step and would be hopeless on any of these interconnects; a DBN
 *  needs two kilobytes.
 *
 *
 *  THE LIFECYCLE OF A NODE
 *  ------------------------
 *      TRAINING  consume input batches, run PCD, watch the error. Emit nothing.
 *      FROZEN    weights fixed. Every input batch is pushed up through the layer and the
 *                hidden activations are emitted to the next node.
 *      A node freezes itself when its error stops improving (bench_dbn_step returns
 *      BENCH_DBN_CONVERGED) or when the orchestrator tells it to.
 *
 *  The upstream node's OUTPUT buffer is bit-for-bit the downstream node's INPUT buffer -- the
 *  same packed ensemble, no conversion, no unpacking, no float anywhere on the wire.
 *
 *
 *  TRANSPORT-AGNOSTIC ON PURPOSE
 *  ------------------------------
 *  Nothing here knows about UART, TCP, ESP-NOW or the framed protocol. The node exposes a
 *  buffer to fill and a buffer to send, and the firmware around it decides how those move.
 *  That is what lets the identical file run on a Teensy behind a UART and a Luckfox behind a
 *  socket, which is the only way a 34-node heterogeneous machine stays maintainable.
 * ===========================================================================================
 */

#ifndef BENCH_DBN_H
#define BENCH_DBN_H

#include "bench_bitslice.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------------------------
 * WIRE FORMAT -- one ensemble batch
 * -----------------------------------------------------------------------------------------
 * Sent as the payload of a framed-protocol message (channel BENCH_CH_WORKER) or a TCP record.
 * Little-endian, packed, 8-byte header then words*replicas uint32.
 *
 * The magic and the shape fields are not padding: a node that receives a batch shaped for a
 * different layer must refuse it rather than train on garbage, and this has to be catchable
 * without a schema negotiation. A stale node left running an old layer size is exactly the
 * failure that would otherwise present as "the network just never converges".
 * ----------------------------------------------------------------------------------------- */
#define BENCH_DBN_MAGIC   0xDB01u

typedef struct {
    uint16_t magic;        /* BENCH_DBN_MAGIC                                                */
    uint8_t  layer;        /* which layer produced this (0 = raw data)                       */
    uint8_t  replicas;     /* must equal BS_REPLICAS on the receiver                          */
    uint16_t units;        /* units in this activation vector                                 */
    uint16_t words;        /* BS_WORDS(units); redundant, but makes a bad header obvious      */
    /* followed by: uint32_t data[replicas * words] */
} bench_dbn_hdr_t;

#define BENCH_DBN_PAYLOAD_BYTES(units) \
    (sizeof(bench_dbn_hdr_t) + (size_t)BS_REPLICAS * BS_WORDS(units) * 4u)


/* -------------------------------------------------------------------------------------------
 * NODE STATE
 * ----------------------------------------------------------------------------------------- */
typedef enum {
    BENCH_DBN_IDLE = 0,    /* no input yet                                                   */
    BENCH_DBN_TRAINING,    /* consuming batches, running PCD                                 */
    BENCH_DBN_FROZEN,      /* weights fixed, forwarding activations downstream               */
} bench_dbn_state_t;

typedef enum {
    BENCH_DBN_OK = 0,
    BENCH_DBN_NO_INPUT,    /* step() called with nothing to do                               */
    BENCH_DBN_CONVERGED,   /* error has plateaued -- the orchestrator should freeze this node */
    BENCH_DBN_EMITTED,     /* frozen node produced an output batch, ready to send            */
    BENCH_DBN_BAD_HEADER,  /* input rejected: wrong magic/shape                              */
} bench_dbn_result_t;

typedef struct {
    bs_layer_t        rbm;

    /* Ensemble buffers. The caller owns the storage; on a Teensy these are DTCM globals. */
    uint32_t         *vis;      /* [BS_REPLICAS][vw] -- the input batch                      */
    uint32_t         *fantasy;  /* [BS_REPLICAS][vw] -- the PERSISTENT chain, never reset    */
    uint32_t         *recon;    /* [BS_REPLICAS][vw] -- scratch                              */
    uint32_t         *hid;      /* [BS_REPLICAS][hw] -- the OUTPUT batch                     */
    uint32_t         *hscratch; /* [BS_REPLICAS][hw] -- scratch                              */

    bench_dbn_state_t state;
    uint8_t           layer_index;
    int16_t           lr;             /* learning rate in shadow units                       */
    uint8_t           decay_shift;    /* weight decay: w -= w >> decay_shift. 9 is measured   */
    uint8_t           has_input;

    uint32_t          batches;
    uint32_t          err_last;
    uint32_t          err_ema;        /* x256 exponential moving average                     */
    uint32_t          err_best;       /* x256                                                */
    uint16_t          stall;          /* batches since err_best improved                     */
    uint16_t          patience;       /* stall count that declares convergence. 0 disables   */
} bench_dbn_t;


/* -------------------------------------------------------------------------------------------
 * API
 * ----------------------------------------------------------------------------------------- */

/* Set up a node around an already-initialised bs_layer_t. Does not allocate. */
void bench_dbn_init(bench_dbn_t *n, uint8_t layer_index,
                    uint32_t *vis, uint32_t *fantasy, uint32_t *recon,
                    uint32_t *hid, uint32_t *hscratch);

/* Accept a batch that arrived on the wire. `payload` points at the header.
 * Returns BENCH_DBN_BAD_HEADER and changes nothing if the shape does not match this layer. */
bench_dbn_result_t bench_dbn_input(bench_dbn_t *n, const void *payload, size_t len);

/* Do one unit of work with whatever input is loaded:
 *   TRAINING -> one PCD step + decay + rethreshold. Returns OK, or CONVERGED once the error
 *               has failed to improve for `patience` batches.
 *   FROZEN   -> push the input up through the layer into `hid`. Returns EMITTED.
 * Either way the input is consumed; call bench_dbn_input() again before the next step. */
bench_dbn_result_t bench_dbn_step(bench_dbn_t *n);

/* After EMITTED: build the wire payload for the next node in `out`. Returns bytes written,
 * or 0 if `cap` is too small. */
size_t bench_dbn_output(const bench_dbn_t *n, void *out, size_t cap);

/* Stop training. Called by the orchestrator, or by the node itself on CONVERGED.
 * Freezing runs one last threshold and gain update so the frozen layer is self-consistent. */
void bench_dbn_freeze(bench_dbn_t *n);

/* Generate a sample from this layer's model and leave it in `vis` (annealed, see
 * bs_generate). Only meaningful once the layer has learned something; this is how you look
 * at what a node believes. */
void bench_dbn_dream(bench_dbn_t *n);

/* Bytes this node's output batch will occupy on the wire. */
size_t bench_dbn_output_bytes(const bench_dbn_t *n);

#ifdef __cplusplus
}
#endif
#endif /* BENCH_DBN_H */
