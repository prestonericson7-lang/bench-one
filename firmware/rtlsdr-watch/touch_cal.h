// Touch calibration: a full 3-point AFFINE map that corrects the FT3168's raw
// coordinate frame to the 410x502 screen. This handles offset, scale, per-axis
// flip, axis swap and rotation (the general linear case), unlike a per-axis fit.
//   screenX = a*rawX + b*rawY + c
//   screenY = d*rawX + e*rawY + f
// The Touch app taps three non-collinear crosshairs, solves the six coefficients,
// writes them to /touchcal.dat on the SD, and applies them live; boot reloads.
#pragma once
#include <Arduino.h>

void  tc_begin(void);                                             // load from SD (identity if none)
void  tc_apply(int32_t rawx, int32_t rawy, int32_t *sx, int32_t *sy);
void  tc_set(float a, float b, float c, float d, float e, float f);
bool  tc_save(float a, float b, float c, float d, float e, float f);   // -> SD, and applies live
bool  tc_calibrated(void);
