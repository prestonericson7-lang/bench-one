// gpu_defs.vh -- mirror of common/gpu_proto.h constants for the RTL. Keep in sync: sim/lint.sh
// compiles gpu_proto.h and compares every value defined here (except T_BUSY_FREE, PL-internal).
`ifndef GPU_DEFS_VH
`define GPU_DEFS_VH

`define GPU_W            1280
`define GPU_H            720
`define GPU_STRIP_H      16
`define GPU_NSTRIPS      45
`define GPU_ROW_WORDS    320          // 64-bit words per row (4 px each)
`define GPU_STRIP_WORDS  5120         // 320 * 16
`define GPU_STRIP_BYTES  32'h0000A000 // 40960
`define GPU_FRAME_WORDS  230400       // 1280*720/4

`define GPU_REC_WORDS    24
`define GPU_LIST_SLOTS   1536

`define GPU_REC_NOP      4'd0
`define GPU_REC_TRI      4'd1
`define GPU_REC_SPRITE   4'd2
`define GPU_REC_END      4'd15

// w0 flag bits
`define GPU_FB_ZTEST     24
`define GPU_FB_ZWRITE    25
`define GPU_FB_NOEDGE    27
`define GPU_FB_COLORKEY  24

`define GPU_ID_VALUE      32'h47505531
`define GPU_VERSION_VALUE 32'h00010000

// register offsets (byte address [11:0])
`define R_ID             12'h000
`define R_VERSION        12'h004
`define R_CONTROL        12'h008
`define R_STATUS         12'h00C
`define R_FRAME_COUNT    12'h010
`define R_FB0            12'h014
`define R_FB1            12'h018
`define R_FRONT          12'h01C
`define R_CLEAR_COLOR    12'h020
`define R_PS_FIFO_FREE   12'h024
`define R_T_FIFO_LEVEL   12'h028
`define R_LIST_OVERFLOW  12'h02C
`define R_BAD_RECORDS    12'h030
`define R_T_WORDS        12'h034
`define R_RENDER_CYCLES  12'h038
`define R_PRIM_COUNT     12'h03C
`define R_VSYNC_COUNT    12'h040
`define R_AXI_ERRORS     12'h044
`define R_DROPPED        12'h048
// SPEC 13.1 return capture (gpu_proto.h GPU_R_RET_ADDR .. GPU_R_LAST_FRAME_NO)
`define R_RET_ADDR       12'h04C
`define R_RET_CTRL       12'h050
`define R_RET_STATUS     12'h054
`define R_RET_FRAME      12'h058
`define R_LAST_FRAME_NO  12'h05C
`define R_PS_FIFO_DATA   12'h100
`define R_PS_FIFO_SOR    12'h104

`define CONTROL_RESET    32'h00000004
`define FB0_RESET        32'h1E000000
`define FB1_RESET        32'h1E200000
`define RET_ADDR_RESET   32'h1FE00000 // gpu_proto.h GPU_RET_ADDR

`define T_BUSY_FREE      64

`endif
