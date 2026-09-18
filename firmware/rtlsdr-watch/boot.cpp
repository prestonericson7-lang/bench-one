// Boot animation: the wedjat (Eye of Horus / Eye of Ra) revealed top-to-bottom
// by a gold scan line, so it looks like it's being drawn onto the panel, then a
// fade to the home screen. The eye is a real raster image (see boot_eye.cpp),
// not vector-drawn at runtime -- the old 360x340 PSRAM canvas is gone (it was the
// prime suspect for the half-lit panel).
#include "ui.h"

// The embedded wedjat, RGB565, baked on black. Defined in boot_eye.cpp.
extern const lv_image_dsc_t wedjat_eye_img;

#define EYE_W 320
#define EYE_H 233

static lv_obj_t *scr = NULL;

static void set_y(void *o, int32_t v) { lv_obj_set_y((lv_obj_t *)o, v); }

static void go_home(lv_timer_t *t) {
  lv_screen_load_anim(ui_home_screen(), LV_SCR_LOAD_ANIM_FADE_ON, 450, 0, false);
  lv_obj_t *dead = scr; scr = NULL;
  if (dead) lv_obj_delete_delayed(dead, 800);
  lv_timer_delete(t);
}

void boot_play(void) {
  scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(scr, 0, LV_PART_MAIN);

  const int ex = (SCREEN_W - EYE_W) / 2;   // eye top-left on screen
  const int ey = 92;

  lv_obj_t *img = lv_image_create(scr);
  lv_image_set_src(img, &wedjat_eye_img);
  lv_obj_set_pos(img, ex, ey);

  // reveal: an opaque black cover the exact size of the eye, slid downward so the
  // eye appears from the top down, with a bright scan line riding its edge.
  lv_obj_t *cover = lv_obj_create(scr);
  lv_obj_set_size(cover, EYE_W, EYE_H);
  lv_obj_set_pos(cover, ex, ey);
  lv_obj_set_style_bg_color(cover, lv_color_hex(0x000000), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(cover, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(cover, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(cover, 0, LV_PART_MAIN);
  lv_obj_remove_flag(cover, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *scan = lv_obj_create(scr);
  lv_obj_set_size(scan, EYE_W, 3);
  lv_obj_set_pos(scan, ex, ey);
  lv_obj_set_style_bg_color(scan, lv_color_hex(0xF2C64A), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(scan, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(scan, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_color(scan, lv_color_hex(0xF2C64A), LV_PART_MAIN);
  lv_obj_set_style_shadow_width(scan, 16, LV_PART_MAIN);
  lv_obj_remove_flag(scan, LV_OBJ_FLAG_SCROLLABLE);

  lv_anim_t a; lv_anim_init(&a);
  lv_anim_set_var(&a, cover);
  lv_anim_set_exec_cb(&a, set_y);
  lv_anim_set_values(&a, ey, ey + EYE_H);
  lv_anim_set_time(&a, 1700);
  lv_anim_set_delay(&a, 300);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
  lv_anim_start(&a);

  lv_anim_t s; lv_anim_init(&s);
  lv_anim_set_var(&s, scan);
  lv_anim_set_exec_cb(&s, set_y);
  lv_anim_set_values(&s, ey, ey + EYE_H);
  lv_anim_set_time(&s, 1700);
  lv_anim_set_delay(&s, 300);
  lv_anim_set_path_cb(&s, lv_anim_path_ease_in_out);
  lv_anim_start(&s);

  // wordmark under the eye
  lv_obj_t *t = lv_label_create(scr);
  lv_label_set_text(t, "WEDJAT");
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, ey + EYE_H + 22);
  lv_obj_set_style_text_color(t, lv_color_hex(0xF2C64A), LV_PART_MAIN);
  lv_obj_set_style_text_font(t, &lv_font_montserrat_40, LV_PART_MAIN);
  lv_obj_set_style_opa(t, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_fade_in(t, 500, 1700);

  lv_obj_t *sub = lv_label_create(scr);
  lv_label_set_text(sub, "ONLINE");
  lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, ey + EYE_H + 78);
  lv_obj_set_style_text_color(sub, COL_TEXT_2, LV_PART_MAIN);
  lv_obj_set_style_text_font(sub, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_opa(sub, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_fade_in(sub, 500, 2100);

  lv_screen_load(scr);
  lv_timer_t *tm = lv_timer_create(go_home, 3000, NULL);
  lv_timer_set_repeat_count(tm, 1);
}

// ---------------------------------------------------------------- charge screen
// Shown when the watch powers up from USB while it was off. It does NOT boot the
// full UI -- it just charges and shows status until the user presses PWR.
static lv_obj_t *chg_scr = NULL, *chg_pct = NULL, *chg_state = NULL, *chg_bar = NULL;

void ui_show_charge(void) {
  chg_scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(chg_scr, lv_color_hex(0x000000), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(chg_scr, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_remove_flag(chg_scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(chg_scr, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(chg_scr, 0, LV_PART_MAIN);

  lv_obj_t *bolt = lv_label_create(chg_scr);
  lv_label_set_text(bolt, LV_SYMBOL_CHARGE);
  lv_obj_align(bolt, LV_ALIGN_CENTER, 0, -90);
  lv_obj_set_style_text_color(bolt, COL_GREEN, LV_PART_MAIN);
  lv_obj_set_style_text_font(bolt, &lv_font_montserrat_40, LV_PART_MAIN);

  chg_pct = lv_label_create(chg_scr);
  lv_obj_align(chg_pct, LV_ALIGN_CENTER, 0, -30);
  lv_obj_set_style_text_color(chg_pct, COL_TEXT, LV_PART_MAIN);
  lv_obj_set_style_text_font(chg_pct, &lv_font_montserrat_40, LV_PART_MAIN);
  lv_label_set_text(chg_pct, "--%");

  chg_bar = lv_bar_create(chg_scr);
  lv_obj_set_size(chg_bar, SCREEN_W - 120, 14);
  lv_obj_align(chg_bar, LV_ALIGN_CENTER, 0, 20);
  lv_bar_set_range(chg_bar, 0, 100);
  lv_bar_set_value(chg_bar, 0, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(chg_bar, COL_CARD_HI, LV_PART_MAIN);
  lv_obj_set_style_bg_color(chg_bar, COL_GREEN, LV_PART_INDICATOR);
  lv_obj_set_style_radius(chg_bar, 7, LV_PART_MAIN);
  lv_obj_set_style_radius(chg_bar, 7, LV_PART_INDICATOR);

  chg_state = lv_label_create(chg_scr);
  lv_obj_align(chg_state, LV_ALIGN_CENTER, 0, 56);
  lv_obj_set_style_text_color(chg_state, COL_TEXT_2, LV_PART_MAIN);
  lv_obj_set_style_text_font(chg_state, &lv_font_montserrat_18, LV_PART_MAIN);
  lv_label_set_text(chg_state, "charging");

  lv_obj_t *hint = lv_label_create(chg_scr);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -40);
  lv_obj_set_style_text_color(hint, COL_SEP, LV_PART_MAIN);
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_label_set_text(hint, "press PWR to power on");

  lv_screen_load(chg_scr);
}

void ui_charge_tick(void) {
  if (!chg_pct) return;
  lv_label_set_text_fmt(chg_pct, "%d%%", SYS.battPct);
  lv_bar_set_value(chg_bar, SYS.battPct, LV_ANIM_OFF);
  const char *s = SYS.charging ? "charging"
                : (SYS.battPct >= 100 ? "charged"
                : (SYS.vbus ? "USB connected" : "on battery"));
  lv_label_set_text(chg_state, s);
}

void ui_charge_enter_ui(void) {
  g_charge_mode = false;
  lv_obj_t *dead = chg_scr;
  chg_scr = NULL; chg_pct = chg_state = chg_bar = NULL;
  boot_play();                              // WEDJAT boot animation -> home
  if (dead) lv_obj_delete_delayed(dead, 900);
}
