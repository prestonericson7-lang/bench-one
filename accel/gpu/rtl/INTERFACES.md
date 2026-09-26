# RTL module interfaces (binding)

Verilog-2001, synthesizable, one module per file named after the module. No SystemVerilog, no
vendor IP cores (no XPM, no FIFO Generator): FIFOs and RAMs are inferred from plain Verilog so
the same source simulates in Icarus Verilog 11 and lints in Verilator 4.038. Xilinx primitives
allowed ONLY: MMCME2_ADV, BUFG, OSERDESE2, OBUFDS (sim stubs live in rtl/sim_stubs/).
Constants shared with C are in `rtl/gpu_defs.vh` (mirror of common/gpu_proto.h).
All resets synchronous active-high. `clk` = core 106.25 MHz unless stated.

```verilog
// rtl/sync_fifo.v  -- first-word-fall-through synchronous FIFO, BRAM-inferable
module sync_fifo #(parameter W = 33, parameter AW = 10) (   // depth = 2**AW
    input  wire         clk, rst,
    input  wire         wr, input wire [W-1:0] din,          // write ignored when full
    input  wire         rd,                                  // pop; ignored when empty
    output wire [W-1:0] dout,                                // valid when !empty (FWFT)
    output wire         empty, full,
    output wire [AW:0]  count,                               // used entries
    output wire [AW:0]  free                                 // 2**AW - count
);

// rtl/par_rx.v  -- Teensy bus receiver (SPEC section 3)
module par_rx (
    input  wire        clk, rst,
    input  wire [15:0] pin_d, input wire pin_sor, input wire pin_strobe,   // raw async pins
    output reg         pin_busy,                                           // registered
    output reg         f_wr, output reg [32:0] f_din,                      // {sor_of_word0, word}
    input  wire [10:0] f_free,                                             // Teensy FIFO free
    output reg  [31:0] words_rx,                                           // words pushed
    output reg         active                                              // edge seen in last ~100 ms
);

// rtl/axi_gp_regs.v  -- GP0 AXI3 slave: registers + PS FIFO push (SPEC 9, gpu_proto.h map)
module axi_gp_regs (
    input  wire clk, rst,
    // AXI3 slave (from PS M_AXI_GP0), 32-bit data, 12-bit IDs, address bits [11:0] decoded
    input  wire [31:0] awaddr, input wire [3:0] awlen, input wire [2:0] awsize,
    input  wire [1:0] awburst, input wire [11:0] awid, input wire awvalid, output wire awready,
    input  wire [31:0] wdata, input wire [3:0] wstrb, input wire wlast, input wire [11:0] wid,
    input  wire wvalid, output wire wready,
    output wire [1:0] bresp, output wire [11:0] bid, output wire bvalid, input wire bready,
    input  wire [31:0] araddr, input wire [3:0] arlen, input wire [2:0] arsize,
    input  wire [1:0] arburst, input wire [11:0] arid, input wire arvalid, output wire arready,
    output wire [31:0] rdata, output wire [1:0] rresp, output wire rlast, output wire [11:0] rid,
    output wire rvalid, input wire rready,
    // PS command FIFO write side
    output reg         pf_wr, output reg [32:0] pf_din, input wire [9:0] pf_free,
    // control outputs
    output reg         soft_reset,                     // 1-cycle pulse
    output wire        src_teensy_en, src_ps_en, scanout_en,
    output wire [31:0] fb0_addr, fb1_addr,
    output wire [15:0] clear_color,
    // status inputs (sampled when read)
    input  wire mmcm_locked, raster_busy, hpd, teensy_active, wait_teensy, wait_ps, swap_pending,
    input  wire [31:0] frame_count, input wire front_idx,
    input  wire [10:0] t_fifo_level,
    input  wire [31:0] list_overflow_cnt, bad_record_cnt, t_words, render_cycles, prim_count,
    input  wire [31:0] vsync_count, axi_err_cnt, dropped_cnt
);

// rtl/core_top.v  -- the render core as instantiated by gpu_pl (owner: render core).
//   collector + record ring + strip raster + strip writer (HP1) + sprite reader (HP2) +
//   SPEC 13.1 return capture. clk = core clock (148.75 MHz, SPEC 2). Port list FROZEN.
module core_top #(parameter LIST_SLOTS = 1536) (   // records per list (SPEC 7), <= 2047
    input  wire        clk, rst, soft_reset,        // rst: sync, active high; soft_reset: 1-cycle
                                                    // pulse, same cycle as the FIFO flush
    input  wire        src_teensy_en, src_ps_en,    // CONTROL bits 1 / 2 (levels)
    input  wire [15:0] clear_color,                 // latched at the start of each frame
    input  wire [31:0] fb0_addr, fb1_addr,          // latched at frame start (back = !front)
    // Teensy FIFO read side (FWFT): t_rd pops the word on t_dout; never asserted while t_empty
    input  wire        t_empty, input wire [32:0] t_dout, output wire t_rd,
    // PS FIFO read side (FWFT), same rules
    input  wire        p_empty, input wire [32:0] p_dout, output wire p_rd,
    // swap handshake with scanout (same clock)
    input  wire        front_idx,
    output wire        swap_req,       // level, raised when the back buffer is fully written
                                       // (all B responses in); held until swap_done
    input  wire        swap_done,      // 1-cycle pulse from scanout when front flipped
    // AXI3 write master -> S_AXI_HP1 (64-bit): 16-beat INCR bursts, 128-byte aligned
    output wire [31:0] wr_awaddr, output wire [3:0] wr_awlen, output wire wr_awvalid, input wire wr_awready,
    output wire [63:0] wr_wdata, output wire wr_wlast, output wire wr_wvalid, input wire wr_wready,
    input  wire [1:0]  wr_bresp, input wire wr_bvalid, output wire wr_bready,     // bready = 1
    // AXI3 read master -> S_AXI_HP2 (64-bit): sprite rows, bursts <= 16 beats, split at 4 KB
    output wire [31:0] rd_araddr, output wire [3:0] rd_arlen, output wire rd_arvalid, input wire rd_arready,
    input  wire [63:0] rd_rdata, input wire [1:0] rd_rresp, input wire rd_rlast, input wire rd_rvalid,
    output wire rd_rready,                                                         // rready = 1
    // status (registers / STATUS bits of gpu_proto.h)
    output wire        raster_busy, wait_teensy, wait_ps,
    output wire [31:0] list_overflow_cnt, bad_record_cnt, render_cycles, prim_count, axi_err_cnt,
    output wire [31:0] dropped_cnt,
    // SPEC 13.1 return capture
    input  wire [31:0] ret_addr,       // RET_ADDR (latched at frame start; bits [6:0] ignored)
    input  wire        ret_enable,     // RET_CTRL bit 0 (level)
    input  wire        ret_ack,        // 1-cycle pulse: RET_CTRL written with bit 1 set
    output wire        ret_full,       // RET_STATUS bit 0
    output wire        ret_capturing,  // RET_STATUS bit 1
    output wire [31:0] ret_frame,      // RET_FRAME: frame_no of the captured frame
    output wire [31:0] last_frame_no   // LAST_FRAME_NO: frame_no of the list last swapped in
);
// Constant AXI fields (size=3, burst=INCR, id=0, cache=4'b0011, prot=0, lock=0, qos=0, wstrb=FF,
// wid=0) are tied off in gpu_pl, not ports of the core. Masters use ID 0 only.
// Behaviour notes (details in the rtl/core_*.v headers):
//  * Lists: the two ping-pong lists (up to LIST_SLOTS records each) share a ring of 2048
//    record slots. When the rendered list plus the list being filled would exceed the ring
//    (only possible with > 511 records in the next list while a large list renders), the
//    collector stops popping the FIFOs until the rendered list is freed (back-pressure via
//    BUSY / PS_FIFO_FREE); the rendered result is unchanged.
//  * A soft_reset keeps only the list being rendered (SPEC 7); counters survive it.
//  * Block RAM: 71 RAMB36 (list ring 40, side table 1, colour strip banks 2 x 10, Z bank 10),
//    Vivado 2026.1 OOC, see sim/core_ooc/.
//  * render_cycles = core cycles from taking the list to swap_req (latched at swap_req).
//  * Rasteriser: 8 pixels per clock (128-bit strip words); strip writer: 1 AXI beat per clock.

// rtl/gpu_core.v  -- core_top with the return capture tied off (ret_enable = 0, ret_ack = 0,
// ret_addr = 0; the ret_* outputs are left open). Same parameter and the same ports as
// core_top minus the seven SPEC 13.1 ports. Kept for tests of the original interface; gpu_pl
// instantiates core_top.
module gpu_core #(parameter LIST_SLOTS = 1536) ( /* core_top ports up to dropped_cnt */ );

// rtl/scanout.v  -- HP0 reader + async FIFO + video timing + TMDS + OSERDES (SPEC 8)
module scanout (
    input  wire        clk, rst,                 // core domain
    input  wire        clk_pix, clk_ser, rst_pix,// pixel domain (rst_pix sync to clk_pix)
    input  wire [31:0] fb0_addr, fb1_addr,
    input  wire        scanout_en,               // core domain level
    input  wire        swap_req, output wire swap_done, output wire front_idx,
    output wire [31:0] frame_count, output wire [31:0] vsync_count,   // core domain
    output wire        swap_pending,
    // AXI3 read master -> S_AXI_HP0 (64-bit), core domain
    output wire [31:0] rd_araddr, output wire [3:0] rd_arlen, output wire rd_arvalid, input wire rd_arready,
    input  wire [63:0] rd_rdata, input wire [1:0] rd_rresp, input wire rd_rlast, input wire rd_rvalid,
    output wire rd_rready,
    output wire [31:0] axi_err_cnt,
    // TMDS pads: [0]=data0 blue, [1]=data1 green, [2]=data2 red, [3]=clock
    output wire [3:0]  tmds_p, tmds_n
);

// rtl/clkgen.v
module clkgen (
    input  wire clk50,
    output wire clk, clk_pix, clk_ser,    // 106.25 / 74.375 / 371.875 MHz, BUFG'd
    output wire locked,
    output wire rst, rst_pix              // sync resets, held until locked + 16 cycles
);

// rtl/gpu_pl.v  -- all GPU logic (clkgen, Teensy bus, FIFOs, axi_gp_regs, core_top, scanout,
// LEDs): ports = section 2 pin map + aclk (core clock out) + the four PS7 AXI ports as plain
// signals (gp_* AXI3 slave, hp0_*/hp1_*/hp2_* AXI3 masters, full signal sets, constants driven
// inside). Instantiated by gpu_top (standalone build) and by
// hardware/pz7020-starlite/vivado/system_top.v (the one system bitstream).

// rtl/gpu_top.v -- standalone board top: ports = PS7 DDR/FIXED_IO pass-through + section 2 pin
// map; ps7_bd_wrapper + gpu_pl, port for port
```
