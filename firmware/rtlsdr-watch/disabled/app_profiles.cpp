// Profiles: browse the correlated wardrive database the Fingerprint engine builds.
// Reads /profiles.csv (one row per profile: radios that travel together) and shows
// them as a scrollable list -- the "who's around me and how it correlates" view.
#include "ui.h"
#include "pin_config.h"
#include <SD_MMC.h>

#define SHOW_MAX 60

static lv_obj_t *statusLbl = NULL, *listBox = NULL;

// Split a CSV line into up to 9 fields in place (our profile rows have no commas
// inside fields -- semicolons separate multi-values -- so a plain split is safe).
static void split9(char *line, char *out[9]) {
  int n = 0; out[0] = line;
  for (char *p = line; *p && n < 8; p++) if (*p == ',') { *p = 0; out[++n] = p + 1; }
  for (int i = n + 1; i < 9; i++) out[i] = (char *)"";
}
static char *unquote(char *s) {
  if (s[0] == '"') { s++; int n = strlen(s); if (n && s[n-1] == '"') s[n-1] = 0; }
  return s;
}

void app_profiles_open(lv_obj_t *body) {
  statusLbl = lv_label_create(body);
  lv_obj_set_style_text_color(statusLbl, COL_TEXT_2, LV_PART_MAIN);
  lv_obj_set_style_text_font(statusLbl, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_label_set_text(statusLbl, "reading profiles...");

  listBox = lv_obj_create(body);
  lv_obj_set_width(listBox, LV_PCT(100));
  lv_obj_set_height(listBox, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(listBox, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(listBox, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(listBox, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_row(listBox, 7, LV_PART_MAIN);
  lv_obj_set_flex_flow(listBox, LV_FLEX_FLOW_COLUMN);
  lv_obj_remove_flag(listBox, LV_OBJ_FLAG_SCROLLABLE);

  SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0);
  bool sd = SD_MMC.begin("/sdcard", true, false);
  if (!sd) { lv_label_set_text(statusLbl, "no SD card"); return; }
  if (!SD_MMC.exists("/profiles.csv")) {
    lv_label_set_text(statusLbl, "no profiles yet - run Fingerprint to build them");
    SD_MMC.end(); return;
  }

  File f = SD_MMC.open("/profiles.csv", FILE_READ);
  int shown = 0, total = 0;
  bool header = true;
  while (f && f.available() && shown < SHOW_MAX) {
    String ln = f.readStringUntil('\n');
    ln.trim();
    if (ln.length() == 0) continue;
    if (header) { header = false; continue; }     // skip column header
    total++;
    char buf[400];
    snprintf(buf, sizeof(buf), "%s", ln.c_str());
    char *fld[9]; split9(buf, fld);
    const char *label = unquote(fld[4]);
    const char *count = fld[3];
    const char *types = unquote(fld[5]);
    const char *names = unquote(fld[7]);
    char title[64], sub[160];
    snprintf(title, sizeof(title), "%s  ·  %s radios", label[0] ? label : "profile", count);
    snprintf(sub, sizeof(sub), "%s%s%s", types,
             (names[0] ? "  ·  " : ""), names);
    ui_row(listBox, title, sub);
    shown++;
  }
  if (f) f.close();
  SD_MMC.end();

  if (total == 0) lv_label_set_text(statusLbl, "no profiles yet - run Fingerprint to build them");
  else lv_label_set_text_fmt(statusLbl, "%d profile%s%s", total, total == 1 ? "" : "s",
                             total > SHOW_MAX ? " (showing latest)" : "");
}

void app_profiles_tick(void) {}

void app_profiles_close(void) {
  statusLbl = listBox = NULL;
}
