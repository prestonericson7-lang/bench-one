// Home screen, app shell and navigation.
//
// Home = an Apple-Watch honeycomb: round icons in a hex grid you drag around in
// any direction, with a fish-eye so whatever's under the middle of the screen
// grows and everything else shrinks. A small clock + battery live in the status
// bar at the very top; the name of the centred app shows just under it. No giant
// clock eating the screen, no labels colliding with the next icon.
//
// App navigation (iOS-ish):
//   - swipe right anywhere            -> home
//   - tap / swipe up the home bar     -> home
//   - back chevron (big hit target)   -> home
//   - BOOT button                     -> home
#include "ui.h"
#include "pin_config.h"
#include <math.h>

// Flashlight bitmap (drawn asset in torch_icon.cpp) -- LVGL has no flashlight glyph,
// so the Torch app gets a real drawn icon instead of a symbol.
extern const lv_image_dsc_t torch_icon_img;

// Tint per app.
static lv_color_t c_blue(void){return COL_BLUE;}   static lv_color_t c_indigo(void){return COL_INDIGO;}
static lv_color_t c_green(void){return COL_GREEN;}  static lv_color_t c_orange(void){return COL_ORANGE;}
static lv_color_t c_red(void){return COL_RED;}      static lv_color_t c_violet(void){return COL_VIOLET;}
static lv_color_t c_teal(void){return COL_TEAL;}    static lv_color_t c_yellow(void){return COL_YELLOW;}

const app_t APPS[] = {
  { "Watch",      LV_SYMBOL_HOME,      c_blue,   app_watchface_open,   app_watchface_close,   app_watchface_tick   },
  { "SDR",        LV_SYMBOL_AUDIO,     c_teal,   app_sdr_open,         app_sdr_close,         app_sdr_tick         },
  { "Screen",     LV_SYMBOL_IMAGE,     c_yellow, app_link_open,        app_link_close,        app_link_tick        },
  { "Tail",       LV_SYMBOL_GPS,       c_teal,   app_tail_open,        app_tail_close,        app_tail_tick        },
  { "Probes",     LV_SYMBOL_WIFI,      c_indigo, app_probe_open,       app_probe_close,       app_probe_tick       },
  { "Channels",   LV_SYMBOL_BARS,      c_indigo, app_heatmap_open,     app_heatmap_close,     app_heatmap_tick     },
  { "Exposure",   LV_SYMBOL_EYE_OPEN,  c_violet, app_exposure_open,    app_exposure_close,    app_exposure_tick    },
  { "Handshake",  LV_SYMBOL_DOWNLOAD,  c_violet, app_handshake_open,   app_handshake_close,   app_handshake_tick   },
  { "Deauth Det", LV_SYMBOL_WARNING,   c_green,  app_deauthd_open,     app_deauthd_close,     app_deauthd_tick     },
  { "Ultrasonic", LV_SYMBOL_VOLUME_MAX,c_violet, app_ultra_open,       app_ultra_close,       app_ultra_tick       },
  { "Bug Sweep",  LV_SYMBOL_AUDIO,     c_blue,   app_bugsweep_open,    app_bugsweep_close,    app_bugsweep_tick    },
  { "Evidence",   LV_SYMBOL_SD_CARD,   c_orange, app_evidence_open,    app_evidence_close,    app_evidence_tick    },
  { "Sensors",    LV_SYMBOL_CHARGE,    c_green,  app_sensors_open,     app_sensors_close,     app_sensors_tick     },
  { "Stopwatch",  LV_SYMBOL_LOOP,      c_teal,   app_stopwatch_open,   app_stopwatch_close,   app_stopwatch_tick   },
  { "Torch",      LV_SYMBOL_POWER,     c_yellow, app_torch_open,       app_torch_close,       app_torch_tick       },
  { "Touch",      LV_SYMBOL_EDIT,      c_orange, app_touchcal_open,    app_touchcal_close,    app_touchcal_tick    },
  { "Settings",   LV_SYMBOL_SETTINGS,  c_teal,   app_settings_open,    app_settings_close,    app_settings_tick    },
};
const int APP_COUNT = sizeof(APPS) / sizeof(APPS[0]);

#define HEADER_H   56
#define HOMEBAR_H  38
#define ICO        62            // honeycomb icon diameter (small = fits one render band)

static lv_obj_t *scr_home = NULL;
static lv_obj_t *scr_app  = NULL;
static lv_obj_t *hive = NULL, *lbl_sb_time = NULL, *lbl_sb_batt = NULL;
static lv_obj_t *icons[24];
static int       icoCX[24], icoCY[24], icoScl[24];   // centres + last-applied scale
static int       active   = -1;
static int       pendingOpen = -1;
static bool      pendingHome = false;
static bool      leaving  = false;

int ui_active_app(void) { return active; }

// ---------------------------------------------------------------- helpers
static void style_screen(lv_obj_t *scr) {
  lv_obj_set_style_bg_color(scr, COL_BG, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(scr, 0, LV_PART_MAIN);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t *ui_card(lv_obj_t *parent) {
  lv_obj_t *c = lv_obj_create(parent);
  lv_obj_set_width(c, LV_PCT(100));
  lv_obj_set_height(c, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(c, COL_CARD, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(c, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(c, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(c, 16, LV_PART_MAIN);
  lv_obj_set_style_pad_all(c, 14, LV_PART_MAIN);
  lv_obj_set_style_pad_row(c, 8, LV_PART_MAIN);
  lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
  lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  return c;
}

lv_obj_t *ui_row(lv_obj_t *parent, const char *title, const char *sub) {
  lv_obj_t *r = lv_obj_create(parent);
  lv_obj_set_width(r, LV_PCT(100));
  lv_obj_set_height(r, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(r, COL_CARD, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(r, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(r, 14, LV_PART_MAIN);
  lv_obj_set_style_pad_hor(r, 14, LV_PART_MAIN);
  lv_obj_set_style_pad_ver(r, 10, LV_PART_MAIN);
  lv_obj_set_style_pad_row(r, 2, LV_PART_MAIN);
  lv_obj_set_flex_flow(r, LV_FLEX_FLOW_COLUMN);
  lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE);

  lv_obj_t *t = lv_label_create(r);
  lv_label_set_text(t, title);
  lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
  lv_obj_set_width(t, LV_PCT(78));
  lv_obj_set_style_text_color(t, COL_TEXT, LV_PART_MAIN);
  lv_obj_set_style_text_font(t, &lv_font_montserrat_18, LV_PART_MAIN);

  lv_obj_t *s = lv_label_create(r);
  lv_label_set_text(s, sub);
  lv_obj_set_width(s, LV_PCT(100));
  lv_obj_set_style_text_color(s, COL_TEXT_2, LV_PART_MAIN);
  lv_obj_set_style_text_font(s, &lv_font_montserrat_14, LV_PART_MAIN);
  return r;
}

// ---------------------------------------------------------------- status bar
static void build_statusbar(lv_obj_t *parent, lv_obj_t **outTime, lv_obj_t **outBatt) {
  lv_obj_t *sb = lv_obj_create(parent);
  lv_obj_set_size(sb, LV_PCT(100), STATUSBAR_H);
  lv_obj_align(sb, LV_ALIGN_TOP_MID, 0, SAFE_TOP);
  lv_obj_set_style_bg_opa(sb, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(sb, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_hor(sb, SAFE_X, LV_PART_MAIN);
  lv_obj_set_style_pad_ver(sb, 0, LV_PART_MAIN);
  lv_obj_remove_flag(sb, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(sb, LV_OBJ_FLAG_CLICKABLE);

  lv_obj_t *t = lv_label_create(sb);
  lv_obj_align(t, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_set_style_text_color(t, COL_TEXT, LV_PART_MAIN);
  lv_obj_set_style_text_font(t, &lv_font_montserrat_18, LV_PART_MAIN);
  lv_label_set_text(t, "--:--");

  lv_obj_t *b = lv_label_create(sb);
  lv_obj_align(b, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_obj_set_style_text_color(b, COL_TEXT_2, LV_PART_MAIN);
  lv_obj_set_style_text_font(b, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_label_set_text(b, LV_SYMBOL_BATTERY_FULL);

  if (outTime) *outTime = t;
  if (outBatt) *outBatt = b;
}

static void refresh_statusbar(lv_obj_t *t, lv_obj_t *b) {
  if (t) {
    const char *ap;
    uint8_t h = hour12(SYS.hh, &ap);
    lv_label_set_text_fmt(t, "%u:%02u %s", h, SYS.mm, ap);
  }
  if (!b) return;
  const char *icon = LV_SYMBOL_BATTERY_EMPTY;
  if (SYS.battPct > 85)      icon = LV_SYMBOL_BATTERY_FULL;
  else if (SYS.battPct > 60) icon = LV_SYMBOL_BATTERY_3;
  else if (SYS.battPct > 35) icon = LV_SYMBOL_BATTERY_2;
  else if (SYS.battPct > 12) icon = LV_SYMBOL_BATTERY_1;
  if (SYS.charging) {
    lv_label_set_text_fmt(b, "%d%% %s %s", SYS.battPct, LV_SYMBOL_CHARGE, icon);
    lv_obj_set_style_text_color(b, COL_GREEN, LV_PART_MAIN);
  } else {
    lv_label_set_text_fmt(b, "%d%% %s", SYS.battPct, icon);
    lv_obj_set_style_text_color(b, SYS.battPct <= 12 ? COL_RED : COL_TEXT_2, LV_PART_MAIN);
  }
}

// ---------------------------------------------------------------- navigation
static void go_home_debounced(void) {
  if (active < 0 || leaving) return;
  leaving = true;
  buzz(18);
  pendingHome = true;
}
static void nav_click_cb(lv_event_t *e) { LV_UNUSED(e); go_home_debounced(); }

// Header "go dark" button: arm Stealth Log -> screen off, touch off, scanner keeps
// running and logging. BOOT wakes it. Available from every app.
static void dark_click_cb(lv_event_t *e) { LV_UNUSED(e); buzz(15); g_stealth = true; ui_enter_dark(); }

static void screen_gesture_cb(lv_event_t *e) {
  LV_UNUSED(e);
  lv_indev_t *indev = lv_indev_active();
  if (!indev) return;
  if (lv_indev_get_gesture_dir(indev) == LV_DIR_RIGHT) go_home_debounced();
}
static void homebar_gesture_cb(lv_event_t *e) {
  LV_UNUSED(e);
  lv_indev_t *indev = lv_indev_active();
  if (!indev) return;
  lv_dir_t dir = lv_indev_get_gesture_dir(indev);
  if (dir == LV_DIR_TOP || dir == LV_DIR_RIGHT) go_home_debounced();
}

// ---------------------------------------------------------------- home / honeycomb
static void icon_clicked(lv_event_t *e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  buzz(18);
  pendingOpen = idx;              // defer: never load/delete screens inside an event
}

// Apple-Watch honeycomb: a hex-packed field of round icons you drag in any
// direction, with a fish-eye that grows whatever is under the centre of the
// screen and shrinks the rest. Rebuilt to actually be fast, unlike the version
// that shipped before:
//   - no shadows (they were the big overdraw cost)
//   - 62px icons, so even zoomed (~78px) they fit inside one render band and the
//     transform never spans two flushes -> no multi-pass smear
//   - the fish-eye recompute is throttled to ~40 Hz and only re-applies an icon's
//     scale when it actually changed, instead of rewriting all of them per event
#define FE_MAX 320          // centre scale (256 = 1.0x  ->  ~1.25x)
#define FE_MIN 150          // far scale    (~0.59x)

static void apply_fisheye(void) {
  if (!hive) return;
  static uint32_t lastRun = 0;
  uint32_t now = lv_tick_get();
  if (now - lastRun < 24) return;                 // cap the recompute at ~40 Hz
  lastRun = now;

  int vpw = lv_obj_get_width(hive), vph = lv_obj_get_height(hive);
  int vcx = lv_obj_get_scroll_x(hive) + vpw / 2;
  int vcy = lv_obj_get_scroll_y(hive) + vph / 2;
  long cull = (long)vph * vph;                     // past this, just pin to min

  for (int i = 0; i < APP_COUNT; i++) {
    int dx = icoCX[i] - vcx, dy = icoCY[i] - vcy;
    long d2 = (long)dx * dx + (long)dy * dy;
    int sc;
    if (d2 > cull) sc = FE_MIN;
    else {
      int d = (int)sqrtf((float)d2);
      sc = FE_MAX - d * (FE_MAX - FE_MIN) / 180;
      if (sc > FE_MAX) sc = FE_MAX;
      if (sc < FE_MIN) sc = FE_MIN;
    }
    if (sc > icoScl[i] + 5 || sc < icoScl[i] - 5) { // only redraw on real change
      icoScl[i] = sc;
      lv_obj_set_style_transform_scale(icons[i], sc, LV_PART_MAIN);
    }
  }
}

static void hive_scroll_cb(lv_event_t *e) { LV_UNUSED(e); apply_fisheye(); }

static void build_home(void) {
  scr_home = lv_obj_create(NULL);
  style_screen(scr_home);
  build_statusbar(scr_home, &lbl_sb_time, &lbl_sb_batt);

  const int top_y = SAFE_TOP + STATUSBAR_H + 12;
  hive = lv_obj_create(scr_home);
  lv_obj_set_size(hive, SCREEN_W, SCREEN_H - top_y - SAFE_BOT);
  lv_obj_align(hive, LV_ALIGN_TOP_MID, 0, top_y);
  lv_obj_set_style_bg_opa(hive, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(hive, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(hive, 0, LV_PART_MAIN);
  lv_obj_set_scroll_dir(hive, LV_DIR_ALL);
  lv_obj_set_scrollbar_mode(hive, LV_SCROLLBAR_MODE_OFF);
  lv_obj_add_event_cb(hive, hive_scroll_cb, LV_EVENT_SCROLL, NULL);

  // hex packing: 4 per row, odd rows shifted half a pitch. Pitch is wider than the
  // screen on purpose so the field is bigger than the viewport and actually drags
  // around (the Apple-Watch pan), not just a static grid you can't move.
  const int PX = 92, PY = 86, top = 40;
  int minx = 1 << 20, miny = 1 << 20, maxx = -(1 << 20), maxy = -(1 << 20);
  for (int i = 0; i < APP_COUNT; i++) {
    int r = i / 4, c = i % 4;
    int cnt = APP_COUNT - r * 4; if (cnt > 4) cnt = 4;
    int off = (r & 1) ? PX / 2 : 0;
    int x0  = (cnt < 4) ? (SCREEN_W / 2 - (cnt - 1) * PX / 2)
                        : (SCREEN_W / 2 - 3 * PX / 2 + off);
    int cx = x0 + c * PX;
    int cy = top + r * PY + ICO / 2;
    icoCX[i] = cx; icoCY[i] = cy; icoScl[i] = 256;
    if (cx < minx) minx = cx; if (cx > maxx) maxx = cx;
    if (cy < miny) miny = cy; if (cy > maxy) maxy = cy;

    lv_obj_t *ico = lv_obj_create(hive);
    lv_obj_set_size(ico, ICO, ICO);
    lv_obj_set_pos(ico, cx - ICO / 2, cy - ICO / 2);
    lv_obj_set_style_bg_color(ico, APPS[i].tint(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ico, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(ico, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(ico, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ico, 0, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_x(ico, ICO / 2, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(ico, ICO / 2, LV_PART_MAIN);
    lv_obj_set_style_opa(ico, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_remove_flag(ico, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ico, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ico, icon_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);

    if (!strcmp(APPS[i].name, "Torch")) {
      lv_obj_t *im = lv_image_create(ico);     // real flashlight bitmap, not a symbol
      lv_image_set_src(im, &torch_icon_img);
      lv_obj_center(im);
    } else {
      lv_obj_t *gl = lv_label_create(ico);
      lv_label_set_text(gl, APPS[i].icon);
      lv_obj_center(gl);
      lv_obj_set_style_text_color(gl, COL_TEXT, LV_PART_MAIN);
      lv_obj_set_style_text_font(gl, &lv_font_montserrat_28, LV_PART_MAIN);
    }

    icons[i] = ico;
  }

  // Two invisible corner spacers push the scrollable area out half a screen past
  // the icons on every side, so any icon can be dragged to the centre and the
  // fish-eye works edge to edge.
  int mgx = SCREEN_W / 2, mgy = (SCREEN_H - top_y - SAFE_BOT) / 2;
  for (int s = 0; s < 2; s++) {
    lv_obj_t *sp = lv_obj_create(hive);
    lv_obj_set_size(sp, 2, 2);
    lv_obj_set_pos(sp, s ? (maxx + mgx) : (minx - mgx),
                       s ? (maxy + mgy) : (miny - mgy));
    lv_obj_set_style_bg_opa(sp, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(sp, 0, LV_PART_MAIN);
    lv_obj_remove_flag(sp, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(sp, LV_OBJ_FLAG_SCROLLABLE);
  }

  lv_obj_update_layout(hive);
  if (APP_COUNT > 0) lv_obj_scroll_to_view(icons[APP_COUNT / 2], LV_ANIM_OFF);  // start centred
  apply_fisheye();
}

// ---------------------------------------------------------------- app shell
void ui_open_app(int idx) {
  if (idx < 0 || idx >= APP_COUNT) return;
  if (active >= 0) return;

  scr_app = lv_obj_create(NULL);
  style_screen(scr_app);
  lv_obj_add_event_cb(scr_app, screen_gesture_cb, LV_EVENT_GESTURE, NULL);

  lv_obj_t *hdr = lv_obj_create(scr_app);
  lv_obj_set_size(hdr, LV_PCT(100), HEADER_H);
  lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, SAFE_TOP);
  lv_obj_set_style_bg_opa(hdr, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(hdr, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(hdr, 0, LV_PART_MAIN);
  lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(hdr, LV_OBJ_FLAG_CLICKABLE);

  lv_obj_t *back = lv_obj_create(hdr);
  lv_obj_set_size(back, 96, HEADER_H);
  lv_obj_align(back, LV_ALIGN_LEFT_MID, SAFE_X - 18, 0);
  lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_bg_color(back, COL_CARD_HI, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(back, LV_OPA_COVER, LV_STATE_PRESSED);
  lv_obj_set_style_radius(back, 14, LV_PART_MAIN);
  lv_obj_set_style_border_width(back, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(back, 0, LV_PART_MAIN);
  lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(back, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(back, nav_click_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t *chev = lv_label_create(back);
  lv_label_set_text(chev, LV_SYMBOL_LEFT);
  lv_obj_center(chev);
  lv_obj_set_style_text_color(chev, APPS[idx].tint(), LV_PART_MAIN);
  lv_obj_set_style_text_font(chev, &lv_font_montserrat_28, LV_PART_MAIN);

  lv_obj_t *title = lv_label_create(hdr);
  lv_label_set_text(title, APPS[idx].name);
  lv_obj_center(title);
  lv_obj_set_style_text_color(title, COL_TEXT, LV_PART_MAIN);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, LV_PART_MAIN);

  // "go dark" (stealth) button, right side of the header
  lv_obj_t *darkb = lv_obj_create(hdr);
  lv_obj_set_size(darkb, 74, HEADER_H);
  lv_obj_align(darkb, LV_ALIGN_RIGHT_MID, -(SAFE_X - 18), 0);
  lv_obj_set_style_bg_color(darkb, COL_CARD_HI, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(darkb, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(darkb, LV_OPA_COVER, LV_STATE_PRESSED);
  lv_obj_set_style_radius(darkb, 14, LV_PART_MAIN);
  lv_obj_set_style_border_width(darkb, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(darkb, 0, LV_PART_MAIN);
  lv_obj_add_flag(darkb, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(darkb, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(darkb, dark_click_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *deye = lv_label_create(darkb);
  lv_label_set_text(deye, LV_SYMBOL_EYE_CLOSE);
  lv_obj_center(deye);
  lv_obj_set_style_text_color(deye, COL_TEXT_2, LV_PART_MAIN);
  lv_obj_set_style_text_font(deye, &lv_font_montserrat_22, LV_PART_MAIN);

  lv_obj_t *body = lv_obj_create(scr_app);
  lv_obj_set_size(body, LV_PCT(100), SCREEN_H - SAFE_TOP - HEADER_H - HOMEBAR_H);
  lv_obj_align(body, LV_ALIGN_TOP_MID, 0, SAFE_TOP + HEADER_H);
  lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(body, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_hor(body, 26, LV_PART_MAIN);
  lv_obj_set_style_pad_ver(body, 4, LV_PART_MAIN);
  lv_obj_set_style_pad_row(body, 10, LV_PART_MAIN);
  lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(body, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);
  lv_obj_add_flag(body, LV_OBJ_FLAG_GESTURE_BUBBLE);

  lv_obj_t *homebar = lv_obj_create(scr_app);
  lv_obj_set_size(homebar, LV_PCT(100), HOMEBAR_H);
  lv_obj_align(homebar, LV_ALIGN_BOTTOM_MID, 0, -6);
  lv_obj_set_style_bg_color(homebar, COL_BG, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(homebar, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(homebar, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(homebar, 0, LV_PART_MAIN);
  lv_obj_add_flag(homebar, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(homebar, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(homebar, nav_click_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_add_event_cb(homebar, homebar_gesture_cb, LV_EVENT_GESTURE, NULL);

  lv_obj_t *pill = lv_obj_create(homebar);
  lv_obj_set_size(pill, 150, 6);
  lv_obj_center(pill);
  lv_obj_set_style_bg_color(pill, COL_TEXT, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(pill, LV_OPA_80, LV_PART_MAIN);
  lv_obj_set_style_radius(pill, 3, LV_PART_MAIN);
  lv_obj_set_style_border_width(pill, 0, LV_PART_MAIN);
  lv_obj_remove_flag(pill, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(pill, LV_OBJ_FLAG_SCROLLABLE);

  active  = idx;
  leaving = false;
  APPS[idx].open(body);

  lv_screen_load_anim(scr_app, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
}

void ui_open_app_by_name(const char *name) {
  for (int i = 0; i < APP_COUNT; i++)
    if (!strcmp(APPS[i].name, name)) { pendingOpen = i; return; }
}

void ui_go_home(void) {
  if (active < 0) return;
  int idx = active;
  active = -1;                       // stop ticks before tearing anything down
  APPS[idx].close();

  lv_screen_load_anim(scr_home, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, false);
  if (scr_app) { lv_obj_delete_delayed(scr_app, 400); scr_app = NULL; }
  leaving = false;
  apply_fisheye();
}

// ---------------------------------------------------------------- tick
void ui_tick(void) {
  if (pendingHome) { pendingHome = false; ui_go_home(); }
  else if (pendingOpen >= 0) {
    int i = pendingOpen; pendingOpen = -1;
    lv_obj_t *old = NULL;
    if (active >= 0) {
      int a = active; active = -1; APPS[a].close();
      old = scr_app; scr_app = NULL;        // keep the old screen ALIVE for the transition
    }
    ui_open_app(i);                          // loads the new screen, animating from `old`
    // Delete the old screen only AFTER the load animation, never before it (deleting
    // the active screen then animating from it dereferences freed memory -> the freeze
    // when you tapped a network in Profiler). Mirrors ui_go_home's ordering.
    if (old) lv_obj_delete_delayed(old, 400);
  }

  refresh_statusbar(lbl_sb_time, lbl_sb_batt);

  if (active >= 0) APPS[active].tick();
}

lv_obj_t *ui_home_screen(void) { return scr_home; }

void ui_init(void) {
  build_home();                        // build but don't show yet
  if (g_charge_mode) ui_show_charge(); // powered from USB while off -> charge screen
  else               boot_play();      // normal: WEDJAT boot animation -> home
}
