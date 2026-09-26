// core_ram.v -- simple dual-port RAM: one write port with byte-lane enables, one read port.
// Synchronous read (read-before-write irrelevant: the users never read and write the same
// address in the same cycle), optional output register (OREG=1 -> 2-cycle read latency:
// raddr/re sampled at edge 1, rdata valid after edge 2). Written in the Vivado "simple dual port
// block RAM with byte-wide write enable" style so it infers RAMB36E1/RAMB18E1 (output register
// absorbed into DOB_REG where possible). Plain Verilog-2001; Icarus 11 / Verilator 4.038 clean.
module core_ram #(
    parameter DW    = 64,        // data width
    parameter NBE   = 8,         // number of write-enable lanes (DW/NBE bits each)
    parameter AW    = 13,        // address width
    parameter DEPTH = 5120,      // words (<= 2**AW)
    parameter OREG  = 1          // 1 = extra output register (2-cycle latency)
) (
    input  wire            clk,
    input  wire [NBE-1:0]  we,
    input  wire [AW-1:0]   waddr,
    input  wire [DW-1:0]   wdata,
    input  wire            re,
    input  wire [AW-1:0]   raddr,
    output wire [DW-1:0]   rdata
);
    localparam CW = DW / NBE;

    reg [DW-1:0] mem [0:DEPTH-1];
    reg [DW-1:0] r1;
    reg [DW-1:0] r2;
    integer i;

    always @(posedge clk) begin
        for (i = 0; i < NBE; i = i + 1)
            if (we[i])
                mem[waddr][i*CW +: CW] <= wdata[i*CW +: CW];
    end

    always @(posedge clk) begin
        if (re)
            r1 <= mem[raddr];
    end

    always @(posedge clk) begin
        r2 <= r1;
    end

    assign rdata = (OREG != 0) ? r2 : r1;
endmodule
