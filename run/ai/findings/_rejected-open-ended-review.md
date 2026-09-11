# 01-gemv-int4-review

- model: qwen2.5-coder:14b
- took: 197 s
- when: 20260909-203136

**This is a candidate, not a result. Verify before acting on it.**

---

I have reviewed the provided Verilog code for `gemv_int4.v` and found several potential issues based on the specified criteria. Here are the detailed findings:

1. **Sign Handling**:
   - **File**: `firmware/bench-one/fpga/rtl/gemv_int4.v`
   - **Line**: 150
   - **Expression**: `wire signed [3:0] w = w_data[(g*4) + 3 : (g*4)];`
   - **Issue**: The sign extension of the nibble is handled correctly by treating it as a signed value. However, ensure that this is consistent throughout the code where nibbles are used.
   - **Input to expose**: Provide an input with negative values in the nibbles to verify correct sign handling.

2. **Width**:
   - **File**: `firmware/bench-one/fpga/rtl/gemv_int4.v`
   - **Line**: 153
   - **Expression**: `assign prod[g] = w * a;`
   - **Issue**: The product of a 4-bit signed weight and an 8-bit signed activation should fit within 12 bits. Ensure that no intermediate values overflow before reaching the 32-bit accumulator.
   - **Input to expose**: Provide an input with large values in the weights and activations to verify that the multiplication does not cause overflow.

3. **Ragged Last Beat**:
   - **File**: `firmware/bench-one/fpga/rtl/gemv_int4.v`
   - **Line**: 170
   - **Expression**: `if ((col + i[AW-1:0]) < cols)`
   - **Issue**: This condition ensures that only valid columns contribute to the sum. Ensure that no padding is counted or accumulated.
   - **Input to expose**: Provide an input where `cols` is not a multiple of `LANES` and verify that the final beat does not include padding.

4. **Reset**:
   - **File**: `firmware/bench-one/fpga/rtl/gemv_int4.v`
   - **Line**: 103
   - **Expression**: `busy <= 1'b0;`
   - **Issue**: Ensure that all registers are properly reset before they are used. In this case, the `busy` register is reset correctly.
   - **Input to expose**: Provide an input without a proper reset sequence to verify that registers do not retain old values.

5. **Memory Index Division**:
   - **File**: `firmware/bench-one/fpga/rtl/gemv_int4.v`
   - **Line**: 160
   - **Expression**: `wire [(LANES*8)-1:0] arow = avec[col[AW-1:0] / LANES];`
   - **Issue**: Ensure that the division `col[AW-1:0] / LANES` is correct when `cols` is not a multiple of `LANES`. This should handle cases where `col` might be slightly larger than necessary.
   - **Input to expose**: Provide an input where `cols` is not a multiple of `LANES` and verify that the memory index calculation is accurate.

These findings are based on the provided code and the specified criteria. Each issue has been identified with a specific line, expression, and potential input to expose the problem.
