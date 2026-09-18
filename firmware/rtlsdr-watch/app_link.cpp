// Screen — control what the E32R40T bridge shows, independently of the SDR app.
// The Teensy keeps running whatever it was last told; this app only flips the
// E32R40T's view (auto-follow / spectrum / detections / telemetry / tuned freq),
// so you can command the SDR in one app, switch here, and watch the bridge confirm
// what the engine is actually doing.
#include "ui.h"
#include "sdr_link.h"

static lv_obj_t *lblStat = NULL, *lblSel = NULL;

static const char* scrn_name(int s) {
  switch (s) {
    case DISP_SPECTRUM:  return "Spectrum";
    case DISP_DETECT:    return "Detections";
    case DISP_TELEMETRY: return "Telemetry";
    case DISP_BIGTUNE:   return "Tuned freq";
    default:             return "Auto (follow SDR)";
  }
}

static lv_obj_t* big_btn(lv_obj_t *parent, const char *txt, lv_event_cb_t cb, void *ud) {
  lv_obj_t *b = lv_obj_create(parent);
  lv_obj_set_size(b, LV_PCT(100), 60);
  lv_obj_set_style_bg_color(b, COL_CARD_HI, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(b, LV_OPA_70, LV_STATE_PRESSED);
  lv_obj_set_style_border_width(b, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(b, 12, LV_PART_MAIN);
  lv_obj_set_style_pad_all(b, 2, LV_PART_MAIN);
  lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
  lv_obj_t *l = lv_label_create(b);
  lv_label_set_text(l, txt);
  lv_obj_center(l);
  lv_obj_set_style_text_color(l, COL_TEXT, LV_PART_MAIN);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_16, LV_PART_MAIN);
  return b;
}

static void pick_cb(lv_event_t *e) {
  int s = (int)(intptr_t)lv_event_get_user_data(e);
  sdr_cmd_u8(CMD_DISP_SCREEN, (uint8_t)s);
  if (lblSel) lv_label_set_text_fmt(lblSel, "E32R40T: %s", scrn_name(s));
  buzz(15);
}

void app_link_open(lv_obj_t *body) {
  sdr_link_begin();

  lv_obj_t *sc = ui_card(body);
  lv_obj_t *t = lv_label_create(sc);
  lv_label_set_text(t, "E32R40T SCREEN");
  lv_obj_set_style_text_color(t, COL_TEAL, LV_PART_MAIN);
  lv_obj_set_style_text_font(t, &lv_font_montserrat_16, LV_PART_MAIN);
  lblStat = lv_label_create(sc);
  lv_label_set_long_mode(lblStat, LV_LABEL_LONG_WRAP); lv_obj_set_width(lblStat, LV_PCT(100));
  lv_obj_set_style_text_color(lblStat, COL_TEXT, LV_PART_MAIN);
  lv_obj_set_style_text_font(lblStat, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_label_set_text(lblStat, "connecting\xE2\x80\xA6");
  lblSel = lv_label_create(sc);
  lv_obj_set_style_text_color(lblSel, COL_YELLOW, LV_PART_MAIN);
  lv_obj_set_style_text_font(lblSel, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_label_set_text(lblSel, "E32R40T: Auto (follow SDR)");

  lv_obj_t *cc = ui_card(body);
  lv_obj_t *ct = lv_label_create(cc);
  lv_label_set_text(ct, "SHOW ON E32R40T");
  lv_obj_set_style_text_color(ct, COL_GREEN, LV_PART_MAIN);
  lv_obj_set_style_text_font(ct, &lv_font_montserrat_14, LV_PART_MAIN);
  big_btn(cc, "Auto (follow SDR)", pick_cb, (void*)(intptr_t)DISP_AUTO);
  big_btn(cc, "Spectrum",          pick_cb, (void*)(intptr_t)DISP_SPECTRUM);
  big_btn(cc, "Detections",        pick_cb, (void*)(intptr_t)DISP_DETECT);
  big_btn(cc, "Telemetry",         pick_cb, (void*)(intptr_t)DISP_TELEMETRY);
  big_btn(cc, "Tuned freq",        pick_cb, (void*)(intptr_t)DISP_BIGTUNE);

  sdr_send_req(REQ_HELLO);
  sdr_send_req(REQ_TELEMETRY);
}

void app_link_tick(void) {
  sdr_link_poll();
  const SdrState *s = sdr_link_state();
  if (lblStat) {
    if (s->linkUp) {
      lv_label_set_text_fmt(lblStat, LV_SYMBOL_OK "  link up \xC2\xB7 %lu frames", (unsigned long)s->frames);
      lv_obj_set_style_text_color(lblStat, COL_GREEN, LV_PART_MAIN);
    } else {
      lv_label_set_text(lblStat, LV_SYMBOL_WARNING "  link down \xE2\x80\x94 is the E32R40T powered?");
      lv_obj_set_style_text_color(lblStat, COL_RED, LV_PART_MAIN);
    }
  }
}

void app_link_close(void) {
  sdr_link_end();
  lblStat = lblSel = NULL;
}
