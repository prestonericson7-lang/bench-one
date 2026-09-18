// Sensors + power: every measurable thing on this board.
//   QMI8658  - 6-axis accel/gyro + die temperature
//   AXP2101  - battery / VBUS / system rails, charge state, die temperature
//   ESP32-S3 - internal die temperature
//   PCF85063 - real-time clock
// There is no magnetometer, barometer, ambient-light or heart-rate sensor on
// this board, so there is no compass, altimeter or auto-brightness to offer.
#include "ui.h"
#include <math.h>

static lv_obj_t *lblAcc = NULL, *lblGyr = NULL, *lblTilt = NULL, *tiltArc = NULL;
static lv_obj_t *lblVolt = NULL, *lblPct = NULL, *lblChg = NULL, *battBar = NULL;
static lv_obj_t *lblVbus = NULL, *lblSys = NULL;
static lv_obj_t *lblTime = NULL, *lblDate = NULL;

static void card_title(lv_obj_t *card, const char *txt, lv_color_t c) {
  lv_obj_t *l = lv_label_create(card);
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_color(l, c, LV_PART_MAIN);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_14, LV_PART_MAIN);
}

static lv_obj_t *card_value(lv_obj_t *card, const char *txt, const lv_font_t *f, lv_color_t c) {
  lv_obj_t *l = lv_label_create(card);
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_color(l, c, LV_PART_MAIN);
  lv_obj_set_style_text_font(l, f, LV_PART_MAIN);
  return l;
}

static void card_note(lv_obj_t *card, const char *txt) {
  lv_obj_t *n = lv_label_create(card);
  lv_label_set_text(n, txt);
  lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(n, LV_PCT(100));
  lv_obj_set_style_text_color(n, COL_SEP, LV_PART_MAIN);
  lv_obj_set_style_text_font(n, &lv_font_montserrat_12, LV_PART_MAIN);
}

void app_sensors_open(lv_obj_t *body) {
  // ---------------------------------------------------------------- motion
  lv_obj_t *m = ui_card(body);
  card_title(m, "MOTION  ·  QMI8658", COL_GREEN);
  if (!SYS.imuOk) {
    card_value(m, "not detected", &lv_font_montserrat_18, COL_RED);
  } else {
    tiltArc = lv_arc_create(m);
    lv_obj_set_size(tiltArc, 108, 108);
    lv_arc_set_rotation(tiltArc, 270);
    lv_arc_set_bg_angles(tiltArc, 0, 360);
    lv_arc_set_range(tiltArc, 0, 90);
    lv_arc_set_value(tiltArc, 0);
    lv_obj_remove_flag(tiltArc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_color(tiltArc, COL_CARD_HI, LV_PART_MAIN);
    lv_obj_set_style_arc_color(tiltArc, COL_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(tiltArc, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_width(tiltArc, 10, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(tiltArc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(tiltArc, 0, LV_PART_KNOB);

    lblTilt = lv_label_create(tiltArc);
    lv_obj_center(lblTilt);
    lv_obj_set_style_text_color(lblTilt, COL_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(lblTilt, &lv_font_montserrat_22, LV_PART_MAIN);
    lv_label_set_text(lblTilt, "0");

    lblAcc = card_value(m, "accel  --", &lv_font_montserrat_16, COL_TEXT);
    lblGyr = card_value(m, "gyro   --", &lv_font_montserrat_16, COL_TEXT_2);
  }

  // ---------------------------------------------------------------- power
  lv_obj_t *p = ui_card(body);
  card_title(p, "POWER  ·  AXP2101", COL_YELLOW);
  if (!SYS.pmuOk) {
    card_value(p, "not detected", &lv_font_montserrat_18, COL_RED);
  } else {
    lblPct = card_value(p, "--%", &lv_font_montserrat_40, COL_TEXT);

    battBar = lv_bar_create(p);
    lv_obj_set_size(battBar, LV_PCT(100), 10);
    lv_bar_set_range(battBar, 0, 100);
    lv_bar_set_value(battBar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(battBar, COL_CARD_HI, LV_PART_MAIN);
    lv_obj_set_style_bg_color(battBar, COL_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_radius(battBar, 5, LV_PART_MAIN);
    lv_obj_set_style_radius(battBar, 5, LV_PART_INDICATOR);

    lblChg  = card_value(p, "--", &lv_font_montserrat_16, COL_TEXT);
    lblVolt = card_value(p, "battery  -- mV", &lv_font_montserrat_16, COL_TEXT_2);
    lblVbus = card_value(p, "VBUS     -- mV", &lv_font_montserrat_16, COL_TEXT_2);
    lblSys  = card_value(p, "system   -- mV", &lv_font_montserrat_16, COL_TEXT_2);
    card_note(p, "percentage is derived from voltage, so it jumps when load changes or the "
                 "charger is plugged in. the millivolts are the number to trust.");
  }

  // ---------------------------------------------------------------- clock
  lv_obj_t *c = ui_card(body);
  card_title(c, "CLOCK  ·  PCF85063", COL_BLUE);
  if (!SYS.rtcOk) {
    card_value(c, "not detected", &lv_font_montserrat_18, COL_RED);
  } else {
    lblTime = card_value(c, "--:--:--", &lv_font_montserrat_32, COL_TEXT);
    lblDate = card_value(c, "--", &lv_font_montserrat_16, COL_TEXT_2);
    card_note(c, "battery-backed from the PMIC's always-on rail. the ESP32 itself has no "
                 "32.768 kHz crystal - those pins are used for I2C and I2S - so this chip "
                 "is the only accurate timebase on the board.");
  }

}

void app_sensors_tick(void) {
  float ax, ay, az, gx, gy, gz;
  if (lblAcc && sys_imu_read(&ax, &ay, &az, &gx, &gy, &gz)) {
    lv_label_set_text_fmt(lblAcc, "accel  %.2f  %.2f  %.2f g", ax, ay, az);
    lv_label_set_text_fmt(lblGyr, "gyro   %.1f  %.1f  %.1f dps", gx, gy, gz);

    float mag = sqrtf(ax * ax + ay * ay + az * az);
    if (mag > 0.01f) {
      float t = acosf(fabsf(az) / mag) * 57.2957795f;
      if (t < 0)  t = 0;
      if (t > 90) t = 90;
      if (tiltArc) lv_arc_set_value(tiltArc, (int)t);
      if (lblTilt) lv_label_set_text_fmt(lblTilt, "%d", (int)t);
    }
  }

  if (lblPct) {
    lv_label_set_text_fmt(lblPct, "%d%%", SYS.battPct);
    lv_bar_set_value(battBar, SYS.battPct, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(battBar,
        SYS.battPct <= 15 ? COL_RED : (SYS.battPct <= 35 ? COL_YELLOW : COL_GREEN),
        LV_PART_INDICATOR);

    if (SYS.charging) {
      lv_label_set_text(lblChg, LV_SYMBOL_CHARGE " charging");
      lv_obj_set_style_text_color(lblChg, COL_GREEN, LV_PART_MAIN);
    } else if (SYS.vbus) {
      lv_label_set_text(lblChg, "USB connected");
      lv_obj_set_style_text_color(lblChg, COL_TEXT, LV_PART_MAIN);
    } else {
      lv_label_set_text(lblChg, "on battery");
      lv_obj_set_style_text_color(lblChg, COL_TEXT_2, LV_PART_MAIN);
    }

    lv_label_set_text_fmt(lblVolt, "battery  %.0f mV", SYS.battVolt);
    lv_label_set_text_fmt(lblVbus, "VBUS     %u mV", SYS.vbusVolt);
    lv_label_set_text_fmt(lblSys,  "system   %u mV", SYS.sysVolt);
  }

  if (lblTime) {
    const char *ap;
    uint8_t h = hour12(SYS.hh, &ap);
    lv_label_set_text_fmt(lblTime, "%u:%02u:%02u %s", h, SYS.mm, SYS.ss, ap);
    lv_label_set_text_fmt(lblDate, "%04u-%02u-%02u", SYS.year, SYS.month, SYS.day);
  }
}

void app_sensors_close(void) {
  lblAcc = lblGyr = lblTilt = tiltArc = NULL;
  lblVolt = lblPct = lblChg = battBar = NULL;
  lblVbus = lblSys = NULL;
  lblTime = lblDate = NULL;
}
