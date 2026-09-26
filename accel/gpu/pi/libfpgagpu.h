/*
 * libfpgagpu.h -- Orange Pi host library for the FPGA-GPU (SPEC sections 11, 13.4, 14).
 *
 *   gpu_conn     TCP connection to the Zynq daemon fpgagpud (port 7777; controller or observer)
 *   teensy_conn  connection to the Teensy 4.1 geometry engine: USB CDC tty (raw termios,
 *                autodetected: /dev/serial/by-id/ *Teensy*, then /dev/ttyACM*), or "tcp:host:port"
 *                for the x86 simulator teensy_sim, or "none"
 *
 * Every NET_* and T_* message of common/gpu_proto.h has a wrapper. All I/O is blocking with
 * timeouts (poll on non-blocking descriptors); TCP uses TCP_NODELAY. Messages to one peer are
 * strictly request/reply in order: one thread per connection. Wire format = host structs, so the
 * library requires a little-endian host (checked at compile time).
 *
 * Return values: 0 = OK, GPU_ERR_* (< 0, the peer's reply status), or FGPU_ERR_* (< 0, local:
 * connection, timeout, malformed reply). After FGPU_ERR_IO / FGPU_ERR_LTIMEOUT the connection is
 * unusable (the byte stream may be out of step) and every later call fails with FGPU_ERR_IO:
 * close it and connect again. gpu_errmsg() / teensy_errmsg() describe the last local failure.
 */
#ifndef LIBFPGAGPU_H
#define LIBFPGAGPU_H

#include <stddef.h>
#include <stdint.h>
#include "gpu_proto.h"
#include "gpu_setup.h"
#include "geom.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Where the Zynq is when no address is given: tried in this order (all at once, first preferred) */
#define FGPU_DEFAULT_HOST     "10.77.0.2"   /* direct cable, pi_setup.sh profile (Pi 10.77.0.1) */
#define FGPU_DEFAULT_HOST2    "10.20.0.2"   /* car LAN, deploy/orangepi car-lan profile (Pi 10.20.0.1) */
#define FGPU_HOST_ENV         "FPGAGPU_HOST" /* HOST or HOST:PORT; replaces the two defaults when set */
#define FGPU_AUTO_GRACE_MS    150           /* ms a later default waits for an earlier one to answer */
#define FGPU_DEFAULT_TIMEOUT  5000          /* ms: reply timeout for ordinary requests */
#define FGPU_CONNECT_TIMEOUT  3000          /* ms: TCP connect */

/* local error codes (the GPU_ERR_* codes of gpu_proto.h are -1..-6) */
#define FGPU_ERR_IO        (-100)   /* connection failed / lost / unusable */
#define FGPU_ERR_LTIMEOUT  (-101)   /* the peer did not answer within the local timeout */
#define FGPU_ERR_REPLY     (-102)   /* malformed or unexpected reply */
#define FGPU_ERR_STATE     (-103)   /* call not valid now (e.g. a *_recv without a *_send) */
#define FGPU_ERR_NOMEM     (-104)   /* local out of memory */
#define FGPU_ERR_ARG       (-105)   /* bad argument (checked locally before sending) */

const char *fgpu_strerror(int code);       /* text for GPU_ERR_* and FGPU_ERR_* */

/* ============================================================================================ */
/* Zynq daemon (TCP 7777)                                                                         */
/* ============================================================================================ */
typedef struct gpu_conn gpu_conn;

/* Connect to host:port (IPv4 dotted quad, "localhost", or a name from /etc/hosts; port <= 0 ->
 * 7777). host NULL or "": $FPGAGPU_HOST (HOST or HOST:PORT; its port is used when port <= 0) if
 * set, else 10.77.0.2 and 10.20.0.2 are tried at the same time and 10.77.0.2 wins unless it has not
 * answered within FGPU_AUTO_GRACE_MS of 10.20.0.2 (the daemon listens on every Zynq address).
 * Then send NET_HELLO: observer = 0 -> controller (the daemon soft-resets the PL, sets
 * CONTROL = SRC_PS|SCANOUT_EN and resets the sprite pool), 1 -> observer. Returns NULL with a
 * message in err on failure. A reply status GPU_ERR_NOPL (no bitstream) is NOT a failure here:
 * check gpu_hello_info(g)->status. */
gpu_conn *gpu_connect(const char *host, int port, int observer, char *err, size_t errlen);
void      gpu_close(gpu_conn *g);
const net_hello_reply *gpu_hello_info(const gpu_conn *g);
const char *gpu_errmsg(const gpu_conn *g);
int       gpu_fd(const gpu_conn *g);
void      gpu_set_timeout(gpu_conn *g, int ms);
int       gpu_is_observer(const gpu_conn *g);

/* NET_HELLO again (change role). r may be NULL. */
int gpu_hello(gpu_conn *g, int observer, net_hello_reply *r);
/* NET_SET_CONFIG: control = GPU_CTL_SRC_TEENSY | GPU_CTL_SRC_PS | GPU_CTL_SCANOUT_EN */
int gpu_set_config(gpu_conn *g, uint32_t control, uint16_t clear_color);
/* drawing messages (no reply; errors are counted by the daemon and reported by gpu_sync) */
int gpu_tris(gpu_conn *g, const net_tri *t, uint32_t n);                 /* split into messages */
int gpu_rect(gpu_conn *g, int x0, int y0, int x1, int y1, uint16_t rgb565, float z, uint32_t flags);
int gpu_sprite_draw(gpu_conn *g, uint16_t id, int x, int y, int colorkey_en, uint16_t colorkey);
int gpu_records(gpu_conn *g, const uint32_t (*recs)[GPU_REC_WORDS], uint32_t n);
int gpu_end_frame(gpu_conn *g, uint32_t frame_no);
/* NET_SPRITE_UPLOAD: w multiple of 4 (>= 4), h >= 1, id < 256; px = w*h RGB565 row-major. */
int gpu_sprite_upload(gpu_conn *g, uint16_t id, uint16_t w, uint16_t h, const uint16_t *px,
                      net_sprite_upload_reply *r);
/* NET_WAIT_FRAME: until FRAME_COUNT >= target (int32 wrap compare). fc may be NULL. */
int gpu_wait_frame(gpu_conn *g, uint32_t target_frame_count, uint32_t timeout_ms, uint32_t *fc);
int gpu_status(gpu_conn *g, net_status_reply *r);
static inline uint32_t gpu_reg(const net_status_reply *r, uint32_t off) { return r->regs[off / 4u]; }
/* NET_READBACK: front buffer, GPU_W*GPU_H pixels into fb */
int gpu_readback(gpu_conn *g, uint16_t *fb);
int gpu_reset(gpu_conn *g);                            /* NET_RESET (soft reset + sprite pool) */
int gpu_sync(gpu_conn *g, uint32_t *errors_since_last_sync);
/* NET_FRAME_GET (two-way path): waits (daemon side, timeout_ms) for a captured frame with
 * frame_no >= min_frame_no (int32 wrap compare). scale 1 -> 1280x720, 2 -> 640x360.
 * px must hold (GPU_W/scale)*(GPU_H/scale) pixels. info (may be NULL) gets frame_no, w, h. */
int gpu_frame_get(gpu_conn *g, uint32_t min_frame_no, uint32_t scale, uint32_t timeout_ms,
                  uint16_t *px, net_frame_get_reply *info);
/* the same, split: send the request, then collect the reply when it is there (the daemon parks
 * the request; this connection must not send anything else until the reply was received).
 * recv: wait_ms = how long to wait for the reply to start (0 = just check). Returns 1 = reply
 * received (its status in info->status: 0 or e.g. GPU_ERR_TIMEOUT), 0 = not yet, < 0 = error. */
int gpu_frame_get_send(gpu_conn *g, uint32_t min_frame_no, uint32_t scale, uint32_t timeout_ms);
int gpu_frame_get_recv(gpu_conn *g, int wait_ms, uint16_t *px, net_frame_get_reply *info);
int gpu_frame_get_pending(const gpu_conn *g);

/* helper: wait until the frame with this frame_no (its END) is on screen (LAST_FRAME_NO), using
 * STATUS + WAIT_FRAME. 0 = shown, GPU_ERR_TIMEOUT = not within timeout_ms, else an error. */
int gpu_wait_frame_no(gpu_conn *g, uint32_t frame_no, uint32_t timeout_ms);

/* statistics of this connection */
void gpu_io_stats(const gpu_conn *g, unsigned long long *tx_bytes, unsigned long long *rx_bytes);

/* ============================================================================================ */
/* Teensy 4.1 geometry engine (USB CDC serial, or tcp:host:port for teensy_sim)                  */
/* ============================================================================================ */
typedef struct teensy_conn teensy_conn;

/* spec: NULL, "" or "auto" = autodetect (/dev/serial/by-id/ *Teensy*, then /dev/ttyACM* in
 * order); "none" = no Teensy (*out = NULL, returns 0); "tcp:host:port" = simulator; anything
 * else = a tty device path. The port is opened raw (8N1, no flow control, no echo) and
 * exclusive (TIOCEXCL); a T_HELLO handshake checks that a geometry engine answers.
 * Returns 0 (*out set, NULL for "none") or < 0 with a message in err. */
int  teensy_open(const char *spec, teensy_conn **out, char *err, size_t errlen);
void teensy_close(teensy_conn *t);
const char *teensy_errmsg(const teensy_conn *t);
const char *teensy_device(const teensy_conn *t);        /* what was opened */
const t_hello_reply *teensy_hello_info(const teensy_conn *t);
int  teensy_fd(const teensy_conn *t);
void teensy_set_timeout(teensy_conn *t, int ms);

int teensy_hello(teensy_conn *t, t_hello_reply *r);
/* poll T_HELLO until fpga_ready && bus_enabled (FPGA configured, BUSY low >= 10 ms).
 * 0 = ready, GPU_ERR_TIMEOUT = not within timeout_ms (r = last reply). r may be NULL. */
int teensy_wait_ready(teensy_conn *t, int timeout_ms, t_hello_reply *r);
int teensy_mesh(teensy_conn *t, uint16_t id, const geom_vertex *v, uint32_t nverts,
                const uint16_t *idx, uint32_t nidx);
/* T_FRAME: hdr->ndraws draws. r gets the reply. With GEOM_FRAME_RETURN in hdr->flags the reply
 * carries the emitted records: *recs = malloc'ed array of r->nrecs_returned records (caller
 * frees; NULL if none); recs may be NULL to discard them. */
int teensy_frame(teensy_conn *t, const geom_frame_hdr *hdr, const geom_draw *draws,
                 t_frame_reply *r, uint32_t (**recs)[GPU_REC_WORDS]);
int teensy_frame_send(teensy_conn *t, const geom_frame_hdr *hdr, const geom_draw *draws);
int teensy_frame_recv(teensy_conn *t, t_frame_reply *r, uint32_t (**recs)[GPU_REC_WORDS]);
int teensy_frame_pending(const teensy_conn *t);
/* T_RECORDS: raw records onto the parallel bus (split into messages). sent may be NULL. */
int teensy_records(teensy_conn *t, const uint32_t (*recs)[GPU_REC_WORDS], uint32_t n, uint32_t *sent);
int teensy_stats(teensy_conn *t, t_stats_reply *r);
int teensy_bus_mode(teensy_conn *t, uint32_t mode);     /* 0 auto, 1 force off (pins high-Z) */
int teensy_reset(teensy_conn *t);                       /* mesh store cleared, autonomous mode off */
/* T_SCENE: h->nobjs objects, h->noverlay overlay records (TRI/SPRITE/NOP only) */
int teensy_scene(teensy_conn *t, const t_scene_hdr *h, const t_scene_obj *objs,
                 const uint32_t (*overlay)[GPU_REC_WORDS]);
int teensy_auto(teensy_conn *t, uint32_t enable, uint32_t max_fps);

void teensy_io_stats(const teensy_conn *t, unsigned long long *tx_bytes, unsigned long long *rx_bytes);

/* ============================================================================================ */
/* misc                                                                                           */
/* ============================================================================================ */
double fgpu_now(void);                  /* monotonic seconds */
void   fgpu_sleep_ms(int ms);

#ifdef __cplusplus
}
#endif
#endif /* LIBFPGAGPU_H */
