// Watchface: it's a watch, so it should tell time at a glance. Big hours:minutes,
// seconds, weekday + date, and a battery pill. Reads the shared SysState the main
// loop already refreshes from the RTC and PMU -- no hardware of its own.
#include "ui.h"

static lv_obj_t *lblTime = NULL, *lblSec = NULL, *lblDate = NULL, *lblBatt = NULL;

static const char *WDAY[7] = { "Sun","Mon","Tue","Wed","Thu","Fri","Sat" };

// Zeller-ish weekday from Y/M/D (Gregorian). Good enough for a watch.
static int weekday(int y, int m, int d) {
  static const int t[] = {0,3,2,5,0,3,5,1,4,6,2,4};
  if (m < 3) y -= 1;
  int w = (y + y/4 - y/100 + y/400 + t[m-1] + d) % 7;
  return (w + 7) % 7;
}

void app_watchface_open(lv_obj_t *body) {
  lblTime = lv_label_create(body);
  lv_obj_set_width(lblTime, LV_PCT(100));
  lv_obj_set_style_text_align(lblTime, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_style_text_color(lblTime, COL_TEXT, LV_PART_MAIN);
  lv_obj_set_style_text_font(lblTime, &lv_font_montserrat_40, LV_PART_MAIN);
  lv_label_set_text(lblTime, "--:--");

  lblSec = lv_label_create(body);
  lv_obj_set_width(lblSec, LV_PCT(100));
  lv_obj_set_style_text_align(lblSec, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_style_text_color(lblSec, COL_BLUE, LV_PART_MAIN);
  lv_obj_set_style_text_font(lblSec, &lv_font_montserrat_22, LV_PART_MAIN);
  lv_label_set_text(lblSec, "");

  lblDate = lv_label_create(body);
  lv_obj_set_width(lblDate, LV_PCT(100));
  lv_obj_set_style_text_align(lblDate, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_style_text_color(lblDate, COL_TEXT_2, LV_PART_MAIN);
  lv_obj_set_style_text_font(lblDate, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_label_set_text(lblDate, "");

  lv_obj_t *card = ui_card(body);
  lblBatt = lv_label_create(card);
  lv_obj_set_width(lblBatt, LV_PCT(100));
  lv_obj_set_style_text_align(lblBatt, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_style_text_color(lblBatt, COL_GREEN, LV_PART_MAIN);
  lv_obj_set_style_text_font(lblBatt, &lv_font_montserrat_18, LV_PART_MAIN);
  lv_label_set_text(lblBatt, "");
}

void app_watchface_tick(void) {
  if (!lblTime) return;
  const char *ap;
  uint8_t h = hour12(SYS.hh, &ap);
  lv_label_set_text_fmt(lblTime, "%u:%02u %s", h, SYS.mm, ap);
  lv_label_set_text_fmt(lblSec, ":%02u", SYS.ss);

  if (SYS.rtcOk) {
    int w = weekday(SYS.year, SYS.month, SYS.day);
    lv_label_set_text_fmt(lblDate, "%s  %04u-%02u-%02u",
                          WDAY[w], SYS.year, SYS.month, SYS.day);
  } else {
    lv_label_set_text(lblDate, "RTC not detected");
  }

  const char *bolt = SYS.charging ? LV_SYMBOL_CHARGE " " : "";
  lv_label_set_text_fmt(lblBatt, "%s%d%%  ·  %.0f mV", bolt, SYS.battPct, SYS.battVolt);
  lv_obj_set_style_text_color(lblBatt,
      SYS.charging ? COL_GREEN : (SYS.battPct <= 15 ? COL_RED : COL_TEXT), LV_PART_MAIN);
}

void app_watchface_close(void) {
  lblTime = lblSec = lblDate = lblBatt = NULL;
}
