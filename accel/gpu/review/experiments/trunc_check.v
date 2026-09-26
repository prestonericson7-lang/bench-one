// review/experiments/trunc_check.v -- does the lint flow catch the 26-bit HB_HALF trap if
// gpu_top.v's heartbeat constant is recomputed for a 148.75 MHz core clock?
module trunc_check (output wire [25:0] v);
    localparam [25:0] HB_HALF = 26'd74374999;   // 0.5 s at 148.75 MHz: needs 27 bits
    assign v = HB_HALF;
    initial #1 $display("HB_HALF as elaborated = %0d (intended 74374999)", HB_HALF);
endmodule
