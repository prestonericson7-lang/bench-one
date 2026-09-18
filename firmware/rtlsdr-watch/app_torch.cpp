// Torch: turn the whole panel white at full brightness. Dumb, but a real use for
// a screen on your wrist in the dark. Restores the previous brightness on exit.
#include "ui.h"

static lv_obj_t *panel = NULL;
static uint8_t   prevBri = 0xD0;
static bool      full = true;

static void toggle_cb(lv_event_t *e) {
  LV_UNUSED(e);
  full = !full;
  ui_set_brightness(full ? 0xFF : 0x60);
  if (panel) lv_obj_set_style_bg_color(panel, full ? lv_color_hex(0xFFFFFF)
                                                    : lv_color_hex(0x808080), LV_PART_MAIN);
}

void app_torch_open(lv_obj_t *body) {
  prevBri = ui_get_brightness();
  full = true;
  ui_set_brightness(0xFF);

  panel = lv_obj_create(body);
  lv_obj_set_size(panel, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(panel, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(panel, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(panel, 0, LV_PART_MAIN);
  lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(panel, toggle_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t *hint = lv_label_create(panel);
  lv_label_set_text(hint, "tap: dim / full");
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);
  lv_obj_set_style_text_color(hint, lv_color_hex(0x808080), LV_PART_MAIN);
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, LV_PART_MAIN);
}

void app_torch_tick(void) {}

void app_torch_close(void) {
  ui_set_brightness(prevBri);
  panel = NULL;
}
