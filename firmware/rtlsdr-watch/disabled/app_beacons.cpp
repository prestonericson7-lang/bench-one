// Named-SSID beacons: broadcast a curated list of network names from /ssids.txt on
// the SD card, so the air around you reads as a believable set of real APs rather
// than random garbage. This is the "transmit done right" tool -- TRANSMITS, so it's
// your own gear / authorized testing only. Each name keeps a stable fake BSSID.
#include "ui.h"
#include "pin_config.h"
#include "radio.h"
#include <SD_MMC.h>

#define MAXSSID 40
static char    ssids[MAXSSID][33];
static uint8_t bssid[MAXSSID][6];
static int     nSsid = 0;
static bool    running = false;
static uint32_t sent = 0, lastTx = 0;
static int      cursor = 0;

static lv_obj_t *btnLbl = NULL, *stateLbl = NULL, *listLbl = NULL;

static const char *FALLBACK[] = { "xfinitywifi", "NETGEAR47", "Linksys00412",
                                  "ATT-WiFi-8821", "Guest", "TP-Link_2G" };

static void add_ssid(const char *s) {
  if (nSsid >= MAXSSID) return;
  int j = 0;
  for (int i = 0; s[i] && j < 32; i++) { char c = s[i]; if (c >= 32 && c <= 126) ssids[nSsid][j++] = c; }
  ssids[nSsid][j] = 0;
  if (j == 0) return;
  for (int i = 0; i < 6; i++) bssid[nSsid][i] = (uint8_t)random(256);
  bssid[nSsid][0] = (bssid[nSsid][0] & 0xFE) | 0x02;   // locally administered, unicast
  nSsid++;
}

static void load_list(void) {
  nSsid = 0;
  SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0);
  bool sd = SD_MMC.begin("/sdcard", true, false);
  if (sd && SD_MMC.exists("/ssids.txt")) {
    File f = SD_MMC.open("/ssids.txt", FILE_READ);
    while (f && f.available() && nSsid < MAXSSID) {
      String line = f.readStringUntil('\n');
      line.trim();
      if (line.length() == 0 || line[0] == '#') continue;
      add_ssid(line.c_str());
    }
    if (f) f.close();
  }
  if (sd) SD_MMC.end();
  if (nSsid == 0)
    for (unsigned i = 0; i < sizeof(FALLBACK)/sizeof(FALLBACK[0]); i++) add_ssid(FALLBACK[i]);
}

// Build a beacon for ssids[idx] on `chan` into buf; returns length.
static int build_beacon(int idx, uint8_t chan, uint8_t *buf) {
  int n = 0;
  const uint8_t hdr[] = { 0x80,0x00, 0x00,0x00, 0xff,0xff,0xff,0xff,0xff,0xff };
  memcpy(buf, hdr, 10); n = 10;
  memcpy(buf + n, bssid[idx], 6); n += 6;          // addr2 src
  memcpy(buf + n, bssid[idx], 6); n += 6;          // addr3 bssid
  buf[n++] = 0x00; buf[n++] = 0x00;                // seq
  for (int i = 0; i < 8; i++) buf[n++] = 0x00;     // timestamp
  buf[n++] = 0x64; buf[n++] = 0x00;                // beacon interval
  buf[n++] = 0x01; buf[n++] = 0x04;                // capability (ESS)
  int sl = strlen(ssids[idx]); if (sl > 32) sl = 32;
  buf[n++] = 0x00; buf[n++] = (uint8_t)sl;         // SSID tag
  memcpy(buf + n, ssids[idx], sl); n += sl;
  const uint8_t rates[] = { 0x01,0x08, 0x82,0x84,0x8b,0x96,0x24,0x30,0x48,0x6c };
  memcpy(buf + n, rates, sizeof(rates)); n += sizeof(rates);
  buf[n++] = 0x03; buf[n++] = 0x01; buf[n++] = chan;  // DS param (channel)
  return n;
}

static void toggle_cb(lv_event_t *e) {
  LV_UNUSED(e); buzz(15);
  if (running) {
    radio_stop(); running = false;
    if (btnLbl) lv_label_set_text(btnLbl, "Start");
    if (stateLbl) lv_label_set_text(stateLbl, "stopped");
  } else {
    radio_start([](void*, wifi_promiscuous_pkt_type_t){}, false);
    running = true; sent = 0; cursor = 0;
    if (btnLbl) lv_label_set_text(btnLbl, "Stop");
    if (stateLbl) lv_label_set_text(stateLbl, "broadcasting");
  }
}

void app_beacons_open(lv_obj_t *body) {
  running = false; sent = 0; cursor = 0;
  load_list();

  lv_obj_t *warn = lv_label_create(body);
  lv_label_set_long_mode(warn, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(warn, LV_PCT(100));
  lv_obj_set_style_text_color(warn, COL_RED, LV_PART_MAIN);
  lv_obj_set_style_text_font(warn, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_label_set_text(warn, "TRANSMITS. Own gear / authorized testing only.");

  lv_obj_t *btn = lv_obj_create(body);
  lv_obj_set_size(btn, LV_PCT(100), 58);
  lv_obj_set_style_bg_color(btn, COL_ORANGE, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(btn, LV_OPA_70, LV_STATE_PRESSED);
  lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(btn, 14, LV_PART_MAIN);
  lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(btn, toggle_cb, LV_EVENT_CLICKED, NULL);
  btnLbl = lv_label_create(btn);
  lv_label_set_text(btnLbl, "Start"); lv_obj_center(btnLbl);
  lv_obj_set_style_text_color(btnLbl, COL_TEXT, LV_PART_MAIN);
  lv_obj_set_style_text_font(btnLbl, &lv_font_montserrat_18, LV_PART_MAIN);

  lv_obj_t *card = ui_card(body);
  stateLbl = lv_label_create(card);
  lv_obj_set_style_text_color(stateLbl, COL_TEXT, LV_PART_MAIN);
  lv_obj_set_style_text_font(stateLbl, &lv_font_montserrat_18, LV_PART_MAIN);
  lv_label_set_text_fmt(stateLbl, "%d names loaded", nSsid);
  listLbl = lv_label_create(card);
  lv_label_set_long_mode(listLbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(listLbl, LV_PCT(100));
  lv_obj_set_style_text_color(listLbl, COL_TEXT_2, LV_PART_MAIN);
  lv_obj_set_style_text_font(listLbl, &lv_font_montserrat_14, LV_PART_MAIN);
  char buf[400]; int p = 0; buf[0] = 0;
  for (int i = 0; i < nSsid && p < (int)sizeof(buf) - 34; i++)
    p += snprintf(buf + p, sizeof(buf) - p, "%s%s", i ? " · " : "", ssids[i]);
  lv_label_set_text(listLbl, buf);
}

void app_beacons_tick(void) {
  if (!running || nSsid == 0) return;
  uint32_t now = millis();
  if (now - lastTx < 40) return;                 // ~25 frames/sec across the list
  lastTx = now;

  static const uint8_t CH[3] = { 1, 6, 11 };
  uint8_t chan = CH[cursor % 3];
  radio_set_channel(chan);
  uint8_t frame[128];
  int len = build_beacon(cursor % nSsid, chan, frame);
  radio_tx(frame, len);
  sent++;
  cursor++;

  static uint32_t last = 0;
  if (now - last > 400) { last = now;
    if (stateLbl) lv_label_set_text_fmt(stateLbl, "broadcasting %d names · %lu sent",
                                        nSsid, (unsigned long)sent);
  }
}

void app_beacons_close(void) {
  if (running) radio_stop();
  running = false;
  btnLbl = stateLbl = listLbl = NULL;
}
