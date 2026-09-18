// Touch calibration: 3-point affine. Tap-and-hold each crosshair; we pair the known
// screen target with the raw FT3168 reading, then solve the general linear map
//   screenX = a*rawX + b*rawY + c ,  screenY = d*rawX + e*rawY + f
// which corrects offset, scale, flip, axis-swap and rotation. Coefficients go to
// /touchcal.dat on the SD and are applied live. A full-screen overlay on the top
// layer absorbs the (still mis-mapped) LVGL touches so they can't trip the header.
#include "ui.h"
#include "touch_cal.h"

// Three non-collinear screen targets: top-left, top-right, bottom-center.
static const int32_t TGX[3] = { 60, SCREEN_W - 60, SCREEN_W / 2 };
static const int32_t TGY[3] = { 90, 90, SCREEN_H - 90 };

static lv_obj_t *overlay = NULL, *title = NULL, *instr = NULL, *status = NULL, *rawLbl = NULL;
static lv_obj_t *chH = NULL, *chV = NULL;

static int      stage = 0;                 // 0,1,2 = points; 3 = done
static int32_t  rawX[3], rawY[3];
static bool     wasDown = false;
static int32_t  latchX = 0, latchY = 0;
static uint32_t settleUntil = 0;

// Solve one axis: s_i = p*rx_i + q*ry_i + r over 3 points (Cramer's rule).
static bool solve3(const float rx[3], const float ry[3], const float s[3],
                   float *p, float *q, float *r) {
  float det = rx[0]*(ry[1]-ry[2]) - ry[0]*(rx[1]-rx[2]) + (rx[1]*ry[2]-rx[2]*ry[1]);
  if (det > -1e-3f && det < 1e-3f) return false;   // collinear / degenerate
  float inv = 1.0f / det;
  *p = (s[0]*(ry[1]-ry[2]) - ry[0]*(s[1]-s[2]) + (s[1]*ry[2]-s[2]*ry[1])) * inv;
  *q = (rx[0]*(s[1]-s[2]) - s[0]*(rx[1]-rx[2]) + (rx[1]*s[2]-rx[2]*s[1])) * inv;
  *r = (rx[0]*(ry[1]*s[2]-s[1]*ry[2]) - ry[0]*(rx[1]*s[2]-s[1]*rx[2]) + s[0]*(rx[1]*ry[2]-ry[1]*rx[2])) * inv;
  return true;
}

static void place_cross(int32_t sx, int32_t sy) {
  if (chH) lv_obj_set_pos(chH, sx - 20, sy - 1);
  if (chV) lv_obj_set_pos(chV, sx - 1,  sy - 20);
}

static void restart(uint32_t now, const char *why) {
  stage = 0; wasDown = false; settleUntil = now + 800;
  place_cross(TGX[0], TGY[0]);
  if (chH) lv_obj_remove_flag(chH, LV_OBJ_FLAG_HIDDEN);
  if (chV) lv_obj_remove_flag(chV, LV_OBJ_FLAG_HIDDEN);
  if (status) { lv_label_set_text(status, why); lv_obj_set_style_text_color(status, COL_ORANGE, LV_PART_MAIN); }
}

void app_touchcal_open(lv_obj_t *body) {
  LV_UNUSED(body);
  stage = 0; wasDown = false; settleUntil = millis() + 400;

  overlay = lv_obj_create(lv_layer_top());
  lv_obj_set_size(overlay, SCREEN_W, SCREEN_H);
  lv_obj_set_pos(overlay, 0, 0);
  lv_obj_set_style_bg_color(overlay, lv_color_hex(0x000000), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(overlay, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(overlay, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(overlay, 0, LV_PART_MAIN);
  lv_obj_remove_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);     // swallow stray touches

  title = lv_label_create(overlay);
  lv_label_set_text(title, "TOUCH CALIBRATION");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 150);
  lv_obj_set_style_text_color(title, COL_TEAL, LV_PART_MAIN);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_22, LV_PART_MAIN);

  instr = lv_label_create(overlay);
  lv_label_set_long_mode(instr, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(instr, SCREEN_W - 80);
  lv_obj_align(instr, LV_ALIGN_TOP_MID, 0, 195);
  lv_obj_set_style_text_align(instr, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_style_text_color(instr, COL_TEXT, LV_PART_MAIN);
  lv_obj_set_style_text_font(instr, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_label_set_text(instr, "tap and hold the + until it moves");

  status = lv_label_create(overlay);
  lv_obj_align(status, LV_ALIGN_TOP_MID, 0, 250);
  lv_obj_set_style_text_color(status, COL_TEXT_2, LV_PART_MAIN);
  lv_obj_set_style_text_font(status, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_label_set_text(status, "point 1 of 3");

  rawLbl = lv_label_create(overlay);
  lv_obj_align(rawLbl, LV_ALIGN_TOP_MID, 0, 285);
  lv_obj_set_style_text_color(rawLbl, COL_SEP, LV_PART_MAIN);
  lv_obj_set_style_text_font(rawLbl, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_label_set_text(rawLbl, "raw: --");

  chH = lv_obj_create(overlay);
  lv_obj_set_size(chH, 40, 3);
  lv_obj_set_style_bg_color(chH, COL_GREEN, LV_PART_MAIN);
  lv_obj_set_style_border_width(chH, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(chH, 0, LV_PART_MAIN);
  lv_obj_remove_flag(chH, LV_OBJ_FLAG_SCROLLABLE);
  chV = lv_obj_create(overlay);
  lv_obj_set_size(chV, 3, 40);
  lv_obj_set_style_bg_color(chV, COL_GREEN, LV_PART_MAIN);
  lv_obj_set_style_border_width(chV, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(chV, 0, LV_PART_MAIN);
  lv_obj_remove_flag(chV, LV_OBJ_FLAG_SCROLLABLE);

  place_cross(TGX[0], TGY[0]);
}

void app_touchcal_tick(void) {
  if (stage >= 3) return;                     // done
  uint32_t now = millis();
  if (now < settleUntil) return;

  int32_t rx, ry;
  bool down = touch_raw(&rx, &ry);
  if (down) {
    latchX = rx; latchY = ry; wasDown = true;
    if (rawLbl) lv_label_set_text_fmt(rawLbl, "raw: %ld, %ld", (long)rx, (long)ry);
    return;
  }
  if (!wasDown) return;

  // press-then-release: record the last raw seen while down
  wasDown = false;
  rawX[stage] = latchX; rawY[stage] = latchY;
  stage++;
  settleUntil = now + 700;

  if (stage < 3) {
    place_cross(TGX[stage], TGY[stage]);
    if (status) lv_label_set_text_fmt(status, "point %d of 3", stage + 1);
    return;
  }

  // three points captured -> solve the affine map
  float frx[3] = { (float)rawX[0], (float)rawX[1], (float)rawX[2] };
  float fry[3] = { (float)rawY[0], (float)rawY[1], (float)rawY[2] };
  float fsx[3] = { (float)TGX[0],   (float)TGX[1],   (float)TGX[2]   };
  float fsy[3] = { (float)TGY[0],   (float)TGY[1],   (float)TGY[2]   };
  float a, b, c, d, e, f;
  if (!solve3(frx, fry, fsx, &a, &b, &c) || !solve3(frx, fry, fsy, &d, &e, &f)) {
    restart(now, "bad taps - start over, point 1 of 3");
    return;
  }

  bool ok = tc_save(a, b, c, d, e, f);        // save to SD + apply live
  buzz(30);
  if (instr) lv_label_set_text(instr, "calibrated");
  if (status) { lv_label_set_text(status, ok ? "saved to SD - press BOOT to exit"
                                             : "no SD card - press BOOT to exit");
                lv_obj_set_style_text_color(status, ok ? COL_GREEN : COL_ORANGE, LV_PART_MAIN); }
  if (chH) lv_obj_add_flag(chH, LV_OBJ_FLAG_HIDDEN);
  if (chV) lv_obj_add_flag(chV, LV_OBJ_FLAG_HIDDEN);
}

void app_touchcal_close(void) {
  if (overlay) lv_obj_delete(overlay);
  overlay = title = instr = status = rawLbl = chH = chV = NULL;
  stage = 0; wasDown = false;
}
