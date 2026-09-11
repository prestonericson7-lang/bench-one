/* ===========================================================================================
 *  qspi_slave.v -- the Teensy-facing side: look like a quad SPI RAM on FlexSPI2 chip select 1
 * ===========================================================================================
 *
 *  WHY THIS SHAPE
 *  --------------
 *  A Teensy 4.1 has exactly one wide external bus: FlexSPI2 port A, four data lines, on the
 *  bottom-side pads (pin 52 = DATA0, 49 = DATA1, 50 = DATA2, 54 = DATA3, 53 = SCLK, 48 = SS0,
 *  51 = SS1). Verified against the i.MX RT1060 reference manual pad mux tables and the Teensy
 *  core's own pin table. The parallel memory controller is not an option -- it needs 41 pads and
 *  only 16 are bonded out. FlexSPI2's second port is not an option either; its data lines sit on
 *  pads EMC_13 to 16, which appear nowhere in the core.
 *
 *  So four lines it is. Per datasheet tables 38 and 42, with the read strobe looped back through
 *  the DQS pad the bus tops out at 133 MHz single rate or 66 MHz double rate, and four lines at
 *  133 MHz is 66.5 MB/s either way. The 166 MHz row of that table needs the memory device to drive
 *  DQS, and FlexSPI2's DQS is pad EMC_23, which a Teensy 4.1 does not bring out. 66.5 MB/s is
 *  therefore the hard ceiling for anything external on this board, and it is twice what the
 *  onboard PSRAM actually delivers.
 *
 *  The payoff is not only speed. FlexSPI2's memory-mapped window is 240 MB (0x7000_0000 to
 *  0x7EFF_FFFF, reference manual page 35), so the DIMM appears as ordinary pointers over a region
 *  fifteen times larger than the whole PSRAM bank, with no driver in the read path at all: the AHB
 *  bus fetches and the CPU spends its cycles on arithmetic instead of moving bytes. That is the
 *  reason for choosing this bus over bit-banged GPIO, which would be nearly as fast on paper and
 *  would burn every cycle the dot product needs.
 *
 *  THE ONE GENUINELY HARD PART
 *  ---------------------------
 *  FlexSPI issues a command, an address, a FIXED number of dummy cycles, then clocks data out
 *  relentlessly. It cannot be stalled. A DDR3 random read needs ACTIVATE, tRCD, CAS latency and the
 *  burst, about thirteen memory clocks, which is 260 ns at a 50 MHz memory clock. Six dummy cycles
 *  at 133 MHz is 45 ns. The numbers do not meet, so data cannot be fetched inside the gap the stock
 *  PSRAM sequence leaves.
 *
 *  Two things close it, and the Teensy sketch sets both:
 *
 *    1. More dummy cycles. The LUT's dummy field is eight bits, so up to 255 are available.
 *       LAT_CYCLES below is what the gateware assumes and it MUST match the sketch. At 64 cycles
 *       and 133 MHz the gap is 481 ns, longer than a worst-case row miss. Against a 512-byte
 *       transaction that is 64 cycles of overhead on 1024 of payload, under 6 percent.
 *
 *    2. Keeping the link no faster than the memory. Eight DDR3 data lines at a 12.5 MHz memory clock
 *       deliver 25 MB/s, and four FlexSPI lines at 49.5 MHz consume 24.75 MB/s, so the fill stays
 *       ahead of the reader by a small constant margin. Where the link cannot be slowed far enough --
 *       49.5 MHz is the floor for this peripheral -- the transaction is shortened instead so the
 *       dummy window covers the whole fetch. The Teensy sketch sets both.
 *
 *  NOT IMPLEMENTED: read-ahead. An earlier version of this comment described a ping-pong buffer that
 *  prefetched the next chunk while the current one was being clocked out, which would decouple link
 *  speed from memory speed entirely. It does not exist. prefetch_en is carried through the config
 *  command and reaches the bridge, but nothing acts on it yet, and saying otherwise in a comment is
 *  how a design acquires features everyone believes in and nobody built.
 * ======================================================================================== */

`default_nettype none
`timescale 1ns / 1ps

module qspi_slave #(
    /* Dummy cycles the Teensy's LUT inserts between address and data. MUST match the sketch. */
    parameter integer LAT_CYCLES = 64,

    /* Identity for command 0x9F in single-bit mode, so the stock Teensy probe finds us. The core
     * reads four bytes and inspects id & 0xFFFF: 0x5D9D selects the ISSI branch, where bits 23:21
     * encode capacity and 0b100 means 16 MB. Byte sequence 9D 5D 80 00 reads back as 0x00805D9D --
     * low half 0x5D9D, and 0x00805D9D >> 21 = 4. The Teensy therefore believes it found a 16 MB
     * PSRAM and maps it without complaint; the sketch then overrides the size register. */
    parameter [31:0] CHIP_ID = 32'h00805D9D,

    parameter integer BUF_BITS = 11            /* 2 KB buffer, 4 of the HX8K's 16 block RAMs */
) (
    input  wire                 sck,
    /* Asynchronous, from the board reset. Needed because several registers here survive chip select
     * on purpose -- the quad-mode flag and the window base are meant to persist between
     * transactions, exactly as a real part's mode register does -- so chip select cannot be their
     * only initialiser. Without this they come up undefined: quad mode could never be entered, and
     * an undefined window base corrupts every address the bridge computes. */
    input  wire                 rst,
    input  wire                 cs_n,
    input  wire [3:0]           io_i,
    output reg  [3:0]           io_o,
    output reg  [3:0]           io_oe,         /* per line, so single-bit replies drive only IO1 */

    /* buffer port, synchronous read on sck */
    output reg  [BUF_BITS-1:0]  buf_addr,
    input  wire [7:0]           buf_rdata,
    output reg  [7:0]           buf_wdata,
    output reg                  buf_we,

    /* to the DDR3 side; the bridge synchronises and edge-detects these */
    output reg  [31:0]          req_addr,
    output reg                  req_start,
    output reg                  req_is_write,
    output reg  [31:0]          win_base,
    output reg                  prefetch_en,
    /* Read timing the Teensy has calibrated. Command 0xC1 carries four nibbles:
     *     nibbles 0 and 1 : read latency in memory clocks
     *     nibble  2       : sample offset within a memory clock
     *     nibble  3 bit 0 : prefetch enable
     * cfg_load pulses for one sck cycle once all four have arrived. */
    output reg  [7:0]           cfg_rd_latency,
    output reg  [3:0]           cfg_rd_sample,
    /* TOGGLES once per completed config, rather than pulsing.
     *
     * This register lives in the sck domain and the controller lives in the FPGA's. A one-cycle pulse
     * crossing between them can be missed entirely or latched metastable, and the symptom would be a
     * calibration sweep that occasionally reports the wrong answer -- the most expensive possible
     * failure, because it sends you hunting a hardware fault that is not there. A toggle is a level,
     * so a two-flop synchroniser and an edge detector on the far side cannot lose it. The two data
     * fields settle a full sck cycle before the toggle flips, and the synchroniser adds two more
     * clocks on top, so they are long stable by the time the far side looks. */
    output reg                  cfg_stb,
    output wire [BUF_BITS-1:0]  wr_count     /* bytes this transaction delivered, for the bridge */
);

    /* command set, matching what the Teensy core already emits, plus two of our own */
    localparam [7:0] C_QUADRD  = 8'hEB,
                     C_QUADWR  = 8'h38,
                     C_ENTQPI  = 8'h35,
                     C_EXITQPI = 8'hF5,
                     C_RSTEN   = 8'h66,
                     C_RESET   = 8'h99,
                     C_RDID    = 8'h9F,
                     C_WINSET  = 8'hC0,
                     C_CFG     = 8'hC1;

    localparam [3:0] P_CMD = 0, P_ADDR = 1, P_DUMMY = 2, P_RDATA = 3, P_WDATA = 4,
                     P_IDD = 5, P_ID = 6, P_WIN = 7, P_CFGB = 8, P_DEAD = 9;

    reg [3:0]  ph;
    reg [7:0]  cmd;
    reg [5:0]  nib;
    reg [31:0] sh;
    reg [7:0]  lat;
    reg [7:0]  byte_reg;
    reg [31:0] id_sh;

    /* Write pointer, kept separate from buf_addr. buf_addr, buf_wdata and buf_we all reach the memory
     * together one clock after they are assigned, so incrementing buf_addr in the same breath as
     * asserting buf_we stores each byte one slot too high. The pointer advances independently and
     * buf_addr is set to the slot actually being written. */
    reg [BUF_BITS-1:0] wptr;
    assign wr_count = wptr;

    /* A flip-flop has ONE asynchronous reset, not two. Listing both chip select and reset as edge
     * events is legal Verilog and unsynthesisable, and yosys says so rather than picking one. So the
     * command path takes a single combined reset, and the few registers that must SURVIVE chip
     * select -- the quad-mode flag, the window base, the calibration, and this write pointer, which
     * is how the bridge learns the byte count after the fact -- live in their own blocks reset only
     * by rst. */
    wire cmd_rst = cs_n | rst;

    /* Quad mode survives chip select, as a real part's mode bit does, so it lives in its own always
     * block and is cleared only by reset. */
    reg quad;
    always @(posedge sck or posedge rst) begin
        if (rst)
            quad <= 1'b0;
        else if (!cs_n && ph == P_CMD && !quad && nib == 7 && {sh[6:0], io_i[0]} == C_ENTQPI)
            quad <= 1'b1;
        else if (!cs_n && ph == P_CMD && quad && nib == 1 && {sh[3:0], io_i} == C_EXITQPI)
            quad <= 1'b0;
    end

    /* The window base and the prefetch bit also persist across chip select, so they get the same
     * treatment. A stale or undefined window base is the worst failure mode in the whole design:
     * every read lands somewhere unintended and the data looks plausibly corrupt rather than
     * obviously absent. */
    always @(posedge sck or posedge rst) begin
        if (rst) begin
            win_base       <= 32'd0;
            prefetch_en    <= 1'b1;
            cfg_rd_latency <= 8'd0;
            cfg_rd_sample  <= 4'd0;
            cfg_stb        <= 1'b0;
        end else begin
            if (!cs_n) begin
                if (ph == P_WIN && nib == 7) win_base <= {sh[27:0], io_i};
                if (ph == P_CFGB) begin
                    case (nib)
                        0: cfg_rd_latency[7:4] <= io_i;
                        1: cfg_rd_latency[3:0] <= io_i;
                        2: cfg_rd_sample       <= io_i;
                        3: begin
                            prefetch_en <= io_i[0];
                            /* Flip the toggle last, so the far side only ever sees a complete set. */
                            cfg_stb     <= ~cfg_stb;
                        end
                        default: ;
                    endcase
                end
            end
        end
    end

    /* ------------------------------------------------------------------------------------------
     *  Command, address and write data are captured on the RISING edge of sck, where FlexSPI
     *  presents them in single data rate mode.
     * --------------------------------------------------------------------------------------- */
    always @(posedge sck or posedge cmd_rst) begin
        if (cmd_rst) begin
            ph           <= P_CMD;
            nib          <= 0;
            sh           <= 32'd0;
            buf_we       <= 1'b0;
            req_start    <= 1'b0;
            req_addr     <= 32'd0;
            req_is_write <= 1'b0;
            buf_addr     <= {BUF_BITS{1'b0}};
            buf_wdata    <= 8'h00;
            byte_reg     <= 8'h00;
            lat          <= 8'h00;
        end else begin
            buf_we <= 1'b0;

            case (ph)
            P_CMD:
                if (quad) begin
                    sh  <= {sh[27:0], io_i};
                    nib <= nib + 1'b1;
                    if (nib == 1) begin
                        cmd <= {sh[3:0], io_i};
                        nib <= 0;
                        case ({sh[3:0], io_i})
                            C_QUADRD: begin ph <= P_ADDR; req_is_write <= 1'b0; end
                            C_QUADWR: begin ph <= P_ADDR; req_is_write <= 1'b1; end
                            C_WINSET:       ph <= P_WIN;
                            C_CFG:          ph <= P_CFGB;
                            default:        ph <= P_DEAD;
                        endcase
                    end
                end else begin
                    sh  <= {sh[30:0], io_i[0]};
                    nib <= nib + 1'b1;
                    if (nib == 7) begin
                        cmd <= {sh[6:0], io_i[0]};
                        nib <= 0;
                        case ({sh[6:0], io_i[0]})
                            C_RDID:  ph <= P_IDD;
                            default: ph <= P_DEAD;
                        endcase
                    end
                end

            /* 32 address bits, eight nibbles. A 24-bit address would cap us at 16 MB; the window is
             * 240 MB and the stick is 4 GB, so the sketch uses a 32-bit address phase. */
            P_ADDR: begin
                sh  <= {sh[27:0], io_i};
                nib <= nib + 1'b1;
                if (nib == 7) begin
                    req_addr  <= {sh[27:0], io_i};
                    req_start <= 1'b1;
                    nib       <= 0;
                    if (req_is_write) begin
                        ph <= P_WDATA;          /* wptr is cleared in its own block, below */
                    end else begin
                        lat <= LAT_CYCLES - 1;
                        ph  <= P_DUMMY;
                    end
                end
            end

            /* The latency window. Long on purpose: this is where the DRAM access happens. The last
             * two cycles are spent priming the buffer read so byte zero is in hand on entry. */
            P_DUMMY:
                if (lat != 0) begin
                    lat <= lat - 1'b1;
                    if (lat == 2) buf_addr <= {BUF_BITS{1'b0}};
                    if (lat == 1) buf_addr <= {{(BUF_BITS-1){1'b0}}, 1'b1};
                end else begin
                    /* byte 0 is on buf_rdata now. buf_addr must STAY at 1 through the first nibble
                     * pair: the buffer's read is registered, so the byte for an address appears one
                     * clock later, and advancing here would fetch byte 2 where byte 1 belongs and
                     * shift the entire transfer by one byte. */
                    byte_reg <= buf_rdata;
                    ph       <= P_RDATA;
                end

            /* Two nibbles per byte. The next byte is loaded as the low nibble of this one leaves,
             * which keeps the block RAM a full cycle ahead of the shifter. */
            P_RDATA: begin
                nib <= nib + 1'b1;
                if (nib[0]) begin
                    byte_reg <= buf_rdata;
                    buf_addr <= buf_addr + 1'b1;
                end
            end

            P_WDATA: begin
                sh  <= {sh[27:0], io_i};
                nib <= nib + 1'b1;
                if (nib[0]) begin
                    buf_addr  <= wptr;          /* the slot being written, not the next one */
                    buf_wdata <= {sh[3:0], io_i};
                    buf_we    <= 1'b1;
                end
            end

            P_WIN: begin
                sh  <= {sh[27:0], io_i};
                nib <= nib + 1'b1;
                if (nib == 7) ph <= P_DEAD;      /* the value itself is latched above */
            end

            P_CFGB: begin
                nib <= nib + 1'b1;
                if (nib == 3) ph <= P_DEAD;      /* four nibbles; values latched above */
            end

            P_IDD: begin                    /* 0x9F leaves 24 dummy bits before the identity */
                nib <= nib + 1'b1;
                if (nib == 23) begin nib <= 0; ph <= P_ID; end
            end

            P_ID: nib <= nib + 1'b1;

            default: ;                      /* P_DEAD: ignore until deselected */
            endcase
        end
    end

    /* The write pointer: cleared at the start of a write, advanced per byte, and deliberately left
     * standing when chip select rises so the bridge can read the count. */
    always @(posedge sck or posedge rst) begin
        if (rst)
            wptr <= {BUF_BITS{1'b0}};
        else if (!cs_n) begin
            if (ph == P_ADDR && nib == 7 && req_is_write) wptr <= {BUF_BITS{1'b0}};
            else if (ph == P_WDATA && nib[0])             wptr <= wptr + 1'b1;
        end
    end

    /* ------------------------------------------------------------------------------------------
     *  Read data is driven on the FALLING edge of sck so it straddles the rising edge the
     *  controller samples on. The note under datasheet table 37 describes exactly this: the memory
     *  generates read data on the falling edge and FlexSPI samples on the falling edge of its
     *  delayed internal clock. Driving on the falling edge gives a half period of settling on both
     *  sides, which is the margin that makes level shifters and flying leads survivable.
     * --------------------------------------------------------------------------------------- */
    always @(negedge sck or posedge cmd_rst) begin
        if (cmd_rst) begin
            io_oe <= 4'h0;
            io_o  <= 4'h0;
            id_sh <= CHIP_ID;
        end else begin
            case (ph)
            P_RDATA: begin
                io_oe <= 4'hF;
                /* High nibble first, the same order the address phase uses. */
                io_o  <= nib[0] ? byte_reg[3:0] : byte_reg[7:4];
            end
            P_ID: begin
                io_oe <= 4'b0010;           /* single-bit reply rides IO1, the MISO line */
                io_o  <= {2'b00, id_sh[0], 1'b0};
                id_sh <= {1'b0, id_sh[31:1]};
            end
            default: begin
                io_oe <= 4'h0;
                io_o  <= 4'h0;
            end
            endcase
        end
    end

endmodule

`default_nettype wire
