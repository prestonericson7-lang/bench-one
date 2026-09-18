// Settings: the things that used to be hard-coded. Screen brightness, haptics on/
// off, charge current, and a live info readout. Everything here writes through the
// helpers in the .ino so one place owns the hardware.
#include "ui.h"
#include "esp_system.h"

static lv_obj_t *briVal = NULL, *info = NULL, *chgInfo = NULL;
static uint32_t  bootMs = 0;

static void bri_cb(lv_event_t *e) {
  lv_obj_t *s = (lv_obj_t *)lv_event_get_target(e);
  int v = lv_slider_get_value(s);
  ui_set_brightness((uint8_t)v);
  if (briVal) lv_label_set_text_fmt(briVal, "%d", v);
}

static void hap_cb(lv_event_t *e) {
  lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
  g_haptics = lv_obj_has_state(sw, LV_STATE_CHECKED);
  if (g_haptics) buzz(20);
}

static void chg_cb(lv_event_t *e) {
  int ma = (int)(intptr_t)lv_event_get_user_data(e);
  buzz(15);
  ui_set_charge_ma(ma);
  if (chgInfo) lv_label_set_text_fmt(chgInfo, "set: %dmA", ma);
}

static void prot_cb(lv_event_t *e) {
  lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
  g_mode_protection = lv_obj_has_state(sw, LV_STATE_CHECKED);
  buzz(15);
}
static void cloak_cb(lv_event_t *e) { LV_UNUSED(e); buzz(15); g_mode_cloak = true; ui_enter_dark(); }

static lv_obj_t *row_card(lv_obj_t *body, const char *title, lv_color_t c) {
  lv_obj_t *card = ui_card(body);
  lv_obj_t *l = lv_label_create(card);
  lv_label_set_text(l, title);
  lv_obj_set_style_text_color(l, c, LV_PART_MAIN);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_14, LV_PART_MAIN);
  return card;
}

void app_settings_open(lv_obj_t *body) {
  bootMs = millis();

  // ---- brightness ----
  lv_obj_t *bc = row_card(body, "BRIGHTNESS", COL_YELLOW);
  lv_obj_t *slider = lv_slider_create(bc);
  lv_obj_set_width(slider, LV_PCT(100));
  lv_slider_set_range(slider, 20, 255);
  lv_slider_set_value(slider, ui_get_brightness(), LV_ANIM_OFF);
  lv_obj_set_style_bg_color(slider, COL_CARD_HI, LV_PART_MAIN);
  lv_obj_set_style_bg_color(slider, COL_YELLOW, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(slider, COL_YELLOW, LV_PART_KNOB);
  lv_obj_add_event_cb(slider, bri_cb, LV_EVENT_VALUE_CHANGED, NULL);
  briVal = lv_label_create(bc);
  lv_obj_set_style_text_color(briVal, COL_TEXT_2, LV_PART_MAIN);
  lv_obj_set_style_text_font(briVal, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_label_set_text_fmt(briVal, "%d", ui_get_brightness());

  // ---- haptics ----
  lv_obj_t *hc = row_card(body, "HAPTICS", COL_GREEN);
  lv_obj_t *sw = lv_switch_create(hc);
  if (g_haptics) lv_obj_add_state(sw, LV_STATE_CHECKED);
  lv_obj_set_style_bg_color(sw, COL_CARD_HI, LV_PART_MAIN);
  lv_obj_set_style_bg_color(sw, COL_GREEN, LV_PART_INDICATOR | LV_STATE_CHECKED);
  lv_obj_add_event_cb(sw, hap_cb, LV_EVENT_VALUE_CHANGED, NULL);

  // ---- charge current ----
  lv_obj_t *cc = row_card(body, "CHARGE CURRENT  (300 mAh cell)", COL_BLUE);
  lv_obj_t *rowb = lv_obj_create(cc);
  lv_obj_set_size(rowb, LV_PCT(100), 52);
  lv_obj_set_style_bg_opa(rowb, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(rowb, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(rowb, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_column(rowb, 8, LV_PART_MAIN);
  lv_obj_set_flex_flow(rowb, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(rowb, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_remove_flag(rowb, LV_OBJ_FLAG_SCROLLABLE);
  const int mas[3] = { 100, 200, 300 };
  const char *lbls[3] = { "100mA", "200mA", "300mA" };
  for (int i = 0; i < 3; i++) {
    lv_obj_t *b = lv_obj_create(rowb);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_height(b, 48);
    lv_obj_set_style_bg_color(b, COL_CARD_HI, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(b, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(b, 12, LV_PART_MAIN);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, chg_cb, LV_EVENT_CLICKED, (void *)(intptr_t)mas[i]);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, lbls[i]);
    lv_obj_center(l);
    lv_obj_set_style_text_color(l, COL_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, LV_PART_MAIN);
  }

  chgInfo = lv_label_create(cc);
  lv_label_set_text(chgInfo, "set: 200mA");
  lv_obj_set_style_text_color(chgInfo, COL_TEXT_2, LV_PART_MAIN);
  lv_obj_set_style_text_font(chgInfo, &lv_font_montserrat_12, LV_PART_MAIN);

  // ---- modes (stackable) ----
  lv_obj_t *mc = row_card(body, "MODES", COL_VIOLET);
  lv_obj_t *pr = lv_obj_create(mc);
  lv_obj_set_size(pr, LV_PCT(100), 44);
  lv_obj_set_style_bg_opa(pr, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(pr, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(pr, 0, LV_PART_MAIN);
  lv_obj_remove_flag(pr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *prl = lv_label_create(pr);
  lv_label_set_text(prl, "Protection (no TX)");
  lv_obj_align(prl, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_set_style_text_color(prl, COL_TEXT, LV_PART_MAIN);
  lv_obj_set_style_text_font(prl, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_t *prsw = lv_switch_create(pr);
  lv_obj_align(prsw, LV_ALIGN_RIGHT_MID, 0, 0);
  if (g_mode_protection) lv_obj_add_state(prsw, LV_STATE_CHECKED);
  lv_obj_set_style_bg_color(prsw, COL_CARD_HI, LV_PART_MAIN);
  lv_obj_set_style_bg_color(prsw, COL_GREEN, LV_PART_INDICATOR | LV_STATE_CHECKED);
  lv_obj_add_event_cb(prsw, prot_cb, LV_EVENT_VALUE_CHANGED, NULL);

  lv_obj_t *ck = lv_obj_create(mc);
  lv_obj_set_size(ck, LV_PCT(100), 48);
  lv_obj_set_style_bg_color(ck, COL_CARD_HI, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(ck, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(ck, LV_OPA_70, LV_STATE_PRESSED);
  lv_obj_set_style_border_width(ck, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(ck, 12, LV_PART_MAIN);
  lv_obj_remove_flag(ck, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(ck, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(ck, cloak_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *ckl = lv_label_create(ck);
  lv_label_set_text(ckl, LV_SYMBOL_EYE_CLOSE "  Cloak (dark + RX-only)");
  lv_obj_center(ckl);
  lv_obj_set_style_text_color(ckl, COL_TEXT, LV_PART_MAIN);
  lv_obj_set_style_text_font(ckl, &lv_font_montserrat_16, LV_PART_MAIN);

  lv_obj_t *mnote = lv_label_create(mc);
  lv_label_set_long_mode(mnote, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(mnote, LV_PCT(100));
  lv_obj_set_style_text_color(mnote, COL_SEP, LV_PART_MAIN);
  lv_obj_set_style_text_font(mnote, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_label_set_text(mnote, "Modes stack. BOOT wakes from dark. The eye icon at the top of any "
                          "app arms Stealth Log (dark + keep scanning/logging).");

  // ---- info ----
  lv_obj_t *ic = row_card(body, "SYSTEM", COL_TEXT_2);
  info = lv_label_create(ic);
  lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(info, LV_PCT(100));
  lv_obj_set_style_text_color(info, COL_TEXT, LV_PART_MAIN);
  lv_obj_set_style_text_font(info, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_label_set_text(info, "");
}

void app_settings_tick(void) {
  if (!info) return;
  uint32_t up = (millis() - bootMs) / 1000;
  lv_label_set_text_fmt(info,
      "WEDJAT  ·  LVGL %d.%d\nheap %u KB free\nuptime %lum %lus",
      lv_version_major(), lv_version_minor(),
      (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
      (unsigned long)(up / 60), (unsigned long)(up % 60));
}

void app_settings_close(void) {
  briVal = info = chgInfo = NULL;
}
