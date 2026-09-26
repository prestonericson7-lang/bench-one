/*
 * gpu_proto.h -- single source of truth for every binary format in the FPGA-GPU system.
 *
 *   Orange Pi 4 Pro (host)  --TCP 7777 over GbE-->  Zynq PS daemon (fpgagpud)  --GP0 regs/FIFO-->  PL GPU
 *   Orange Pi 4 Pro (host)  --USB serial------->   Teensy 4.1 (geometry)     --16-bit parallel bus--> PL GPU
 *
 * Shared by: RTL (by hand: constants mirrored in rtl/gpu_defs.vh), Zynq daemon, Pi library + tools,
 * Teensy firmware, and the golden reference model. All multi-byte values are little-endian.
 * See SPEC.md for the semantics; this file only defines layouts and constants.
 */
#ifndef GPU_PROTO_H
#define GPU_PROTO_H

#include <stdint.h>

#if defined(__GNUC__)
#define GPU_PACKED __attribute__((packed))
#else
#define GPU_PACKED
#endif

/* ------------------------------------------------------------------------------------------ */
/* Screen, framebuffer, DDR layout                                                             */
/* ------------------------------------------------------------------------------------------ */
#define GPU_W            1280
#define GPU_H            720
#define GPU_STRIP_H      16
#define GPU_NSTRIPS      (GPU_H / GPU_STRIP_H)        /* 45 */
#define GPU_PPW          4                             /* RGB565 pixels per 64-bit word */
#define GPU_ROW_WORDS    (GPU_W / GPU_PPW)             /* 320 */
#define GPU_FB_BYTES     (GPU_W * GPU_H * 2)           /* 1,843,200 */
#define GPU_STRIP_BYTES  (GPU_W * GPU_STRIP_H * 2)     /* 40,960 = 0xA000, 4 KB aligned */

#define GPU_SUBPIX_BITS  4                             /* vertex coords snapped to 1/16 px */
#define GPU_GUARD_PX     256                           /* guard band beyond each screen edge */
#define GPU_GUARD_XMIN   (-GPU_GUARD_PX)
#define GPU_GUARD_XMAX   (GPU_W + GPU_GUARD_PX)        /* exclusive */
#define GPU_GUARD_YMIN   (-GPU_GUARD_PX)
#define GPU_GUARD_YMAX   (GPU_H + GPU_GUARD_PX)        /* exclusive */

#define GPU_Z_FRAC_BITS  12                            /* z fixed point: Z16 << 12 */
#define GPU_C_FRAC_BITS  16                            /* colour fixed point: 0..255 << 16 */

/* DDR region reserved for the GPU by the Zynq device tree (reserved-memory, no-map). */
#define GPU_DDR_BASE     0x1E000000u
#define GPU_DDR_SIZE     0x02000000u                   /* 32 MB */
#define GPU_FB0_ADDR     0x1E000000u
#define GPU_FB1_ADDR     0x1E200000u
#define GPU_POOL_ADDR    0x1E400000u                   /* sprite/image pool */
#define GPU_POOL_SIZE    0x01A00000u                   /* 26 MB: 0x1E400000..0x1FDFFFFF */
#define GPU_RET_ADDR     0x1FE00000u                   /* return-capture buffer (2 MB) */

/* ------------------------------------------------------------------------------------------ */
/* Records (the PL command format). Every record is exactly 24 x 32-bit words.                */
/* ------------------------------------------------------------------------------------------ */
#define GPU_REC_WORDS    24
#define GPU_REC_BYTES    (GPU_REC_WORDS * 4)           /* 96 */
#define GPU_LIST_SLOTS   1536                          /* max TRI+SPRITE records per frame */

#define GPU_REC_NOP      0u
#define GPU_REC_TRI      1u
#define GPU_REC_SPRITE   2u
#define GPU_REC_END      15u

/* w0 layout (all record types): [31:28] type, [27:24] flags, [23:22] 0, [21:11] a, [10:0] b */
#define GPU_W0_TYPE(w0)  (((w0) >> 28) & 0xFu)

/* TRI flags (w0 bits 27:24) */
#define GPU_F_ZTEST      (1u << 24)   /* pass only if zpix <= zbuf */
#define GPU_F_ZWRITE     (1u << 25)   /* on pass, zbuf = zpix */
#define GPU_F_RSVD26     (1u << 26)
#define GPU_F_NOEDGE     (1u << 27)   /* ignore edge equations: every pixel in bbox is covered (rects) */
/* SPRITE flags (w0 bits 27:24) */
#define GPU_F_COLORKEY   (1u << 24)   /* skip source pixels equal to w5[15:0] */

/*
 * TRI record:
 *   w0  = TRI<<28 | flags | xmax<<11 | xmin        (bbox, inclusive, already clamped to screen)
 *   w1  = ymax<<11 | ymin
 *   w2  = A0   w3 = B0   w4 = E0_0               edge 0 (int32 each)
 *   w5  = A1   w6 = B1   w7 = E0_1               edge 1
 *   w8  = A2   w9 = B2   w10 = E0_2              edge 2
 *   w11 = z0   w12 = dzdx   w13 = dzdy           Z16 << 12 (int32)
 *   w14 = r0   w15 = drdx   w16 = drdy           colour << 16 (int32)
 *   w17 = g0   w18 = dgdx   w19 = dgdy
 *   w20 = b0   w21 = dbdx   w22 = dbdy
 *   w23 = 0
 *   For pixel (x,y) in bbox, with dx = x - xmin, dy = y - ymin, all arithmetic modulo 2^32:
 *     E_i = E0_i + A_i*dx + B_i*dy ; covered = NOEDGE || (E_0>=0 && E_1>=0 && E_2>=0) (as int32)
 *     v   = v0 + dvdx*dx + dvdy*dy  for v in {z,r,g,b}
 *
 * SPRITE record:
 *   w0 = SPRITE<<28 | flags
 *   w1 = y<<11 | x          x multiple of 4, 0<=x<1280, 0<=y<720
 *   w2 = h<<11 | w          w multiple of 4, 4<=w<=1280, 1<=h<=720
 *   w3 = src DDR address    8-byte aligned
 *   w4 = src stride bytes   multiple of 8
 *   w5 = colour key (RGB565 in [15:0])
 *   w6..w23 = 0
 *
 * END record:
 *   w0 = END<<28 ; w1 = frame number ; w2..w23 = 0
 */

/* ------------------------------------------------------------------------------------------ */
/* PL register map (AXI GP0 slave). Base address as seen by the Zynq PS.                      */
/* ------------------------------------------------------------------------------------------ */
#define GPU_REGS_PHYS        0x43C00000u
#define GPU_REGS_SIZE        0x1000u

#define GPU_R_ID             0x000   /* RO  0x47505531 'GPU1' */
#define GPU_R_VERSION        0x004   /* RO  0x00010000 */
#define GPU_R_CONTROL        0x008   /* RW  see GPU_CTL_*, reset value 0x00000004 */
#define GPU_R_STATUS         0x00C   /* RO  see GPU_ST_* */
#define GPU_R_FRAME_COUNT    0x010   /* RO  rendered frames made visible (swaps) */
#define GPU_R_FB0            0x014   /* RW  reset GPU_FB0_ADDR */
#define GPU_R_FB1            0x018   /* RW  reset GPU_FB1_ADDR */
#define GPU_R_FRONT          0x01C   /* RO  index (0/1) of the buffer being scanned out */
#define GPU_R_CLEAR_COLOR    0x020   /* RW  RGB565 in [15:0], reset 0 */
#define GPU_R_PS_FIFO_FREE   0x024   /* RO  free 32-bit entries in the PS command FIFO */
#define GPU_R_T_FIFO_LEVEL   0x028   /* RO  used entries in the Teensy command FIFO */
#define GPU_R_LIST_OVERFLOW  0x02C   /* RO  records dropped because the list was full */
#define GPU_R_BAD_RECORDS    0x030   /* RO  malformed / unknown / partial records discarded */
#define GPU_R_T_WORDS        0x034   /* RO  32-bit words received on the Teensy bus */
#define GPU_R_RENDER_CYCLES  0x038   /* RO  core clock cycles to render the last frame */
#define GPU_R_PRIM_COUNT     0x03C   /* RO  records in the last rendered list */
#define GPU_R_VSYNC_COUNT    0x040   /* RO  video frames output since reset */
#define GPU_R_AXI_ERRORS     0x044   /* RO  AXI SLVERR/DECERR responses seen by the PL masters */
#define GPU_R_DROPPED        0x048   /* RO  words drained from a disabled source */
/* Two-way return path (SPEC section 13) */
#define GPU_R_RET_ADDR       0x04C   /* RW  return-capture buffer, reset GPU_RET_ADDR */
#define GPU_R_RET_CTRL       0x050   /* RW  bit0 RET_ENABLE; write bit1=1 -> RET_ACK (self-clearing) */
#define GPU_R_RET_STATUS     0x054   /* RO  bit0 RET_FULL, bit1 RET_CAPTURING */
#define GPU_R_RET_FRAME      0x058   /* RO  frame_no of the frame held in the return buffer */
#define GPU_R_LAST_FRAME_NO  0x05C   /* RO  frame_no of the list most recently made visible */
#define GPU_R_PS_FIFO_DATA   0x100   /* WO  push word (continuation of a record) */
#define GPU_R_PS_FIFO_SOR    0x104   /* WO  push word with start-of-record flag (word 0) */

#define GPU_ID_VALUE         0x47505531u
#define GPU_VERSION_VALUE    0x00010000u

#define GPU_CTL_SOFT_RESET   (1u << 0)   /* write 1: flush FIFOs + collector (self-clearing) */
#define GPU_CTL_SRC_TEENSY   (1u << 1)   /* collector takes a list part from the Teensy bus */
#define GPU_CTL_SRC_PS       (1u << 2)   /* collector takes a list part from the PS FIFO */
#define GPU_CTL_SCANOUT_EN   (1u << 3)   /* 0 = HDMI shows colour bars, 1 = framebuffer */

#define GPU_ST_MMCM_LOCKED   (1u << 0)
#define GPU_ST_RASTER_BUSY   (1u << 1)
#define GPU_ST_HPD           (1u << 2)   /* HDMI hot-plug detect (monitor present) */
#define GPU_ST_TEENSY_ACTIVE (1u << 3)   /* a bus strobe edge was seen in the last ~100 ms */
#define GPU_ST_WAIT_TEENSY   (1u << 4)   /* collector waiting for the Teensy END */
#define GPU_ST_WAIT_PS       (1u << 5)   /* collector waiting for the PS END */
#define GPU_ST_SWAP_PENDING  (1u << 6)

#define GPU_RET_ENABLE       (1u << 0)   /* RET_CTRL */
#define GPU_RET_ACK          (1u << 1)   /* RET_CTRL, write-only pulse */
#define GPU_RET_FULL         (1u << 0)   /* RET_STATUS */
#define GPU_RET_CAPTURING    (1u << 1)   /* RET_STATUS */

#define GPU_PS_FIFO_DEPTH    512
#define GPU_T_FIFO_DEPTH     1024
#define GPU_T_BUSY_FREE      64          /* BUSY=1 while Teensy FIFO free entries < this */

/* Zynq devcfg: PL configured flag (read before touching GP0 space) */
#define ZYNQ_DEVCFG_BASE     0xF8007000u
#define ZYNQ_DEVCFG_INT_STS  0x00Cu
#define ZYNQ_DEVCFG_PCFG_DONE (1u << 2)

/* ------------------------------------------------------------------------------------------ */
/* Message framing (both host links): 12-byte header followed by `len` payload bytes.          */
/* Replies use type | GPU_MSG_REPLY and always start with int32 status (0 = ok, <0 = error).   */
/* ------------------------------------------------------------------------------------------ */
typedef struct GPU_PACKED {
    uint32_t magic;
    uint16_t type;
    uint16_t flags;     /* 0 */
    uint32_t len;       /* payload bytes */
} gpu_msg_hdr;

#define GPU_MSG_REPLY        0x8000u
#define GPU_MSG_MAX_PAYLOAD  (8u * 1024u * 1024u)

#define GPU_ERR_OK           0
#define GPU_ERR_PROTO        -1   /* malformed message */
#define GPU_ERR_NOPL         -2   /* PL not configured / ID mismatch */
#define GPU_ERR_ARG          -3   /* bad argument */
#define GPU_ERR_NOMEM        -4   /* pool / mesh store full */
#define GPU_ERR_TIMEOUT      -5
#define GPU_ERR_BUS          -6   /* Teensy: FPGA bus not ready */

/* ---- Pi <-> Zynq daemon, TCP port 7777 --------------------------------------------------- */
#define GPU_NET_PORT         7777
#define GPU_NET_MAGIC        0x31504746u   /* bytes 'F','G','P','1' */
#define GPU_NET_PROTO_VER    1u

enum {
    NET_HELLO         = 1,   /* -> reply net_hello_reply */
    NET_SET_CONFIG    = 2,   /* net_set_config -> reply status */
    NET_TRIS          = 3,   /* u32 count, count x net_tri            (no reply) */
    NET_RECT          = 4,   /* net_rect                               (no reply) */
    NET_SPRITE_UPLOAD = 5,   /* net_sprite_upload + w*h u16 pixels -> reply net_sprite_upload_reply */
    NET_SPRITE_DRAW   = 6,   /* net_sprite_draw                        (no reply) */
    NET_RECORDS       = 7,   /* u32 count, count x 24 u32              (no reply) */
    NET_END_FRAME     = 8,   /* u32 frame_no                           (no reply) */
    NET_WAIT_FRAME    = 9,   /* net_wait_frame -> reply net_wait_frame_reply */
    NET_STATUS        = 10,  /* -> reply net_status_reply */
    NET_READBACK      = 11,  /* -> reply: status, u32 w, u32 h, w*h u16 (front buffer) */
    NET_RESET         = 12,  /* soft reset + sprite pool reset -> reply status */
    NET_SYNC          = 13,  /* -> reply net_sync_reply (after all earlier messages were processed) */
    NET_FRAME_GET     = 14   /* net_frame_get -> reply net_frame_get_reply + w*h u16 (two-way path) */
};

/* NET_HELLO request payload: empty (= controller) or u32 flags. */
#define GPU_HELLO_OBSERVER   (1u << 0)   /* observer: may only HELLO/STATUS/WAIT_FRAME/READBACK/FRAME_GET/SYNC */

typedef struct GPU_PACKED {
    uint32_t min_frame_no;   /* wait for a captured frame with frame_no >= this (int32 wrap compare) */
    uint32_t scale;          /* 1 = 1280x720, 2 = 640x360 (every 2nd pixel of every 2nd row) */
    uint32_t timeout_ms;
} net_frame_get;

typedef struct GPU_PACKED {
    int32_t  status;
    uint32_t frame_no;       /* frame_no of the returned frame (from its END record) */
    uint32_t w, h;           /* followed by w*h u16 RGB565 pixels, row-major */
} net_frame_get_reply;

typedef struct GPU_PACKED {
    int32_t  status;
    uint32_t proto_version;
    uint32_t pl_id;
    uint32_t pl_version;
    uint32_t width, height;
    uint32_t pool_size;
    uint32_t max_sprites;
} net_hello_reply;

typedef struct GPU_PACKED {
    uint32_t control;       /* GPU_CTL_SRC_TEENSY | GPU_CTL_SRC_PS | GPU_CTL_SCANOUT_EN */
    uint32_t clear_color;   /* RGB565 */
} net_set_config;

typedef struct GPU_PACKED {
    float    x, y, z;       /* x,y pixels (pixel centre = +0.5), z 0..1 */
    uint8_t  r, g, b, a;
} net_vtx;                  /* 16 bytes, identical layout to gpu_vtx */

typedef struct GPU_PACKED {
    net_vtx  v[3];
    uint32_t flags;         /* GPU_F_ZTEST | GPU_F_ZWRITE */
    uint32_t cull;          /* GPU_CULL_* */
} net_tri;                  /* 56 bytes */

typedef struct GPU_PACKED {
    int32_t  x0, y0, x1, y1;   /* half-open */
    uint32_t rgb565;
    float    z;
    uint32_t flags;            /* GPU_F_ZTEST | GPU_F_ZWRITE */
} net_rect;

typedef struct GPU_PACKED {
    uint16_t id, w, h, reserved;   /* w multiple of 4 */
} net_sprite_upload;

typedef struct GPU_PACKED {
    int32_t  status;
    uint32_t ddr_addr;             /* where the pixels now live */
    uint32_t stride;
} net_sprite_upload_reply;

typedef struct GPU_PACKED {
    uint16_t id, flags;            /* flags bit0 = colour key */
    int32_t  x, y;                 /* x multiple of 4 (may be negative) */
    uint32_t colorkey;
} net_sprite_draw;

typedef struct GPU_PACKED {
    uint32_t target_frame_count;
    uint32_t timeout_ms;
} net_wait_frame;

typedef struct GPU_PACKED {
    int32_t  status;
    uint32_t frame_count;
} net_wait_frame_reply;

#define NET_STATUS_NREGS 24        /* registers 0x000..0x05C */
typedef struct GPU_PACKED {
    int32_t  status;
    uint32_t regs[NET_STATUS_NREGS];
    uint32_t daemon_errors;        /* malformed / rejected commands since start */
    uint32_t daemon_records;       /* records pushed to the PL since start */
    uint32_t pool_used;
} net_status_reply;

typedef struct GPU_PACKED {
    int32_t  status;
    uint32_t errors_since_last_sync;
} net_sync_reply;

/* ---- Pi <-> Teensy, USB CDC serial -------------------------------------------------------- */
#define GPU_TUSB_MAGIC       0x31534754u   /* bytes 'T','G','S','1' */
#define GPU_TUSB_FW_VER      1u

enum {
    T_HELLO    = 1,   /* -> reply t_hello_reply */
    T_MESH     = 2,   /* t_mesh_hdr + nverts x geom_vertex + nidx x u16 -> reply status */
    T_FRAME    = 3,   /* geom_frame_hdr + ndraws x geom_draw -> reply t_frame_reply */
    T_RECORDS  = 4,   /* u32 count, count x 24 u32 -> forwarded raw -> reply status (+u32 sent) */
    T_STATS    = 6,   /* -> reply t_stats_reply */
    T_BUS_MODE = 7,   /* u32 mode: 0 auto, 1 force off (pins high-Z) -> reply status */
    T_RESET    = 8,   /* clear mesh store, stop autonomous mode -> reply status */
    T_SCENE    = 9,   /* t_scene_hdr + nobjs x t_scene_obj + noverlay x 24 u32 -> reply status */
    T_AUTO     = 10   /* t_auto -> reply status: start/stop the autonomous render loop */
};

/* ---- Autonomous mode (SPEC section 14): the Teensy animates and renders on its own ---------- */
#define T_MAX_SCENE_OBJS     64
#define T_MAX_OVERLAY_RECS   32

typedef struct GPU_PACKED {
    uint32_t nobjs, noverlay;
    float    view[16];          /* column-major view matrix at t = 0 */
    float    proj[16];          /* column-major projection */
    float    light_dir[3];      /* unit, direction the light travels */
    float    ambient;
    float    cam_orbit_rate;    /* rad/s: camera orbits world Y (view = view0 * rotY(-rate*t)); 0 = static */
} t_scene_hdr;                  /* 156 bytes */

typedef struct GPU_PACKED {
    uint16_t mesh_id, flags;    /* GEOM_DRAW_* */
    float    model[16];         /* base model matrix */
    float    spin_axis[3];      /* object-space unit axis */
    float    spin_rate;         /* rad/s; model(t) = model * rot(spin_axis, spin_rate*t) */
    uint8_t  color_mul[4];
} t_scene_obj;                  /* 88 bytes */

typedef struct GPU_PACKED {
    uint32_t enable;            /* 1 = run, 0 = stop */
    uint32_t max_fps;           /* 0 = as fast as the FPGA accepts */
} t_auto;

typedef struct GPU_PACKED {
    int32_t  status;
    uint32_t fw_version;
    uint32_t max_meshes, max_verts, max_indices;
    uint32_t fpga_ready;     /* 1 = BUSY has been driven low by a configured FPGA */
    uint32_t bus_enabled;    /* 1 = data pins are outputs */
} t_hello_reply;

typedef struct GPU_PACKED {
    uint16_t mesh_id, reserved;
    uint32_t nverts, nidx;
} t_mesh_hdr;

typedef struct GPU_PACKED {
    int32_t  status;
    uint32_t frame_no;
    uint32_t tris_in, tris_out, tris_culled, tris_clipped;
    uint32_t us_total;       /* time spent on this frame */
    uint32_t us_bus_wait;    /* part of that spent waiting for BUSY */
    uint32_t nrecs_returned; /* followed by nrecs_returned x 24 u32 when GEOM_FRAME_RETURN was set */
} t_frame_reply;

typedef struct GPU_PACKED {
    int32_t  status;
    uint32_t frames, records_sent, words_sent;
    uint32_t bus_timeouts, fpga_ready, bus_enabled;
    uint32_t usb_bad_msgs;
    uint32_t auto_running;      /* 1 while autonomous mode renders */
    uint32_t auto_frames;       /* frames produced by autonomous mode */
    uint32_t auto_fps_x100;     /* measured autonomous frame rate * 100 (last second) */
    uint32_t cpu_busy_pct;      /* % of the last second spent computing/sending (not idle) */
    uint32_t tris_per_sec;      /* TRI records sent in the last second */
    uint32_t cpu_mhz;           /* F_CPU / 1e6 */
} t_stats_reply;

#endif /* GPU_PROTO_H */
