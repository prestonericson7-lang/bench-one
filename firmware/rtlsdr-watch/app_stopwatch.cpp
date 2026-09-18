// Stopwatch: start/stop/reset with laps. Uses millis(); no hardware. The motor
// gives a tick on every button so it works eyes-free.
#include "ui.h"

static lv_obj_t *big = NULL, *btnSS = NULL, *btnSSlbl = NULL, *lapBox = NULL;
static bool      running = false;
static uint32_t  accumMs = 0, startMs = 0;
static int       lapN = 0;

static uint32_t elapsed(void) {
  return accumMs + (running ? (millis() - startMs) : 0);
}
static void fmt(uint32_t ms, char *out, int n) {
  uint32_t t = ms / 100;            // tenths
  snprintf(out, n, "%lu:%02lu.%lu", (unsigned long)(t/600),
           (unsigned long)((t/10)%60), (unsigned long)(t%10));
}

static void ss_cb(lv_event_t *e) {
  LV_UNUSED(e); buzz(18);
  if (running) { accumMs = elapsed(); running = false; }
  else         { startMs = millis();  running = true;  }
  if (btnSSlbl) lv_label_set_text(btnSSlbl, running ? "Stop" : "Start");
  if (btnSS) lv_obj_set_style_bg_color(btnSS, running ? COL_RED : COL_GREEN, LV_PART_MAIN);
}

static void reset_cb(lv_event_t *e) {
  LV_UNUSED(e); buzz(18);
  running = false; accumMs = 0; startMs = 0; lapN = 0;
  if (btnSSlbl) lv_label_set_text(btnSSlbl, "Start");
  if (btnSS) lv_obj_set_style_bg_color(btnSS, COL_GREEN, LV_PART_MAIN);
  if (lapBox) lv_obj_clean(lapBox);
  if (big) lv_label_set_text(big, "0:00.0");
}

static void lap_cb(lv_event_t *e) {
  LV_UNUSED(e); buzz(18);
  if (!lapBox || (!running && accumMs == 0)) return;
  char t[16]; fmt(elapsed(), t, sizeof(t));
  char line[32]; snprintf(line, sizeof(line), "lap %d   %s", ++lapN, t);
  lv_obj_t *r = ui_row(lapBox, line, "");
  lv_obj_scroll_to_view(r, LV_ANIM_ON);
}

static lv_obj_t *mk_btn(lv_obj_t *parent, const char *txt, lv_color_t c, lv_event_cb_t cb, lv_obj_t **outLbl) {
  lv_obj_t *b = lv_obj_create(parent);
  lv_obj_set_flex_grow(b, 1);
  lv_obj_set_height(b, 58);
  lv_obj_set_style_bg_color(b, c, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(b, LV_OPA_70, LV_STATE_PRESSED);
  lv_obj_set_style_border_width(b, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(b, 14, LV_PART_MAIN);
  lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *l = lv_label_create(b);
  lv_label_set_text(l, txt); lv_obj_center(l);
  lv_obj_set_style_text_color(l, COL_TEXT, LV_PART_MAIN);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_18, LV_PART_MAIN);
  if (outLbl) *outLbl = l;
  return b;
}

void app_stopwatch_open(lv_obj_t *body) {
  running = false; accumMs = 0; startMs = 0; lapN = 0;

  big = lv_label_create(body);
  lv_obj_set_width(big, LV_PCT(100));
  lv_obj_set_style_text_align(big, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_style_text_color(big, COL_TEXT, LV_PART_MAIN);
  lv_obj_set_style_text_font(big, &lv_font_montserrat_40, LV_PART_MAIN);
  lv_label_set_text(big, "0:00.0");

  lv_obj_t *btns = lv_obj_create(body);
  lv_obj_set_size(btns, LV_PCT(100), 66);
  lv_obj_set_style_bg_opa(btns, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(btns, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(btns, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_column(btns, 8, LV_PART_MAIN);
  lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(btns, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_remove_flag(btns, LV_OBJ_FLAG_SCROLLABLE);
  btnSS = mk_btn(btns, "Start", COL_GREEN, ss_cb, &btnSSlbl);
  mk_btn(btns, "Lap",   COL_CARD_HI, lap_cb, NULL);
  mk_btn(btns, "Reset", COL_CARD_HI, reset_cb, NULL);

  lapBox = lv_obj_create(body);
  lv_obj_set_width(lapBox, LV_PCT(100));
  lv_obj_set_height(lapBox, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(lapBox, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(lapBox, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(lapBox, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_row(lapBox, 6, LV_PART_MAIN);
  lv_obj_set_flex_flow(lapBox, LV_FLEX_FLOW_COLUMN);
  lv_obj_remove_flag(lapBox, LV_OBJ_FLAG_SCROLLABLE);
}

void app_stopwatch_tick(void) {
  if (!big) return;
  char t[16]; fmt(elapsed(), t, sizeof(t));
  lv_label_set_text(big, t);
}

void app_stopwatch_close(void) {
  big = btnSS = btnSSlbl = lapBox = NULL;
}
