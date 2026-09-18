// Evidence logger: sweep WiFi + BLE and append every sighting to a timestamped
// CSV on the SD card. Build a case file over time, pull it later. RTC gives the
// real timestamp (see the clock notes elsewhere).
#include "ui.h"
#include "oui.h"
#include "pin_config.h"
#include <WiFi.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <SD_MMC.h>
#include "ble.h"

static lv_obj_t *statusLbl = NULL, *countLbl = NULL, *lastLbl = NULL, *note = NULL;
static File     csv;
static bool     sdOk = false, bleInit = false;
static BLEScan *scanner = NULL;
static uint32_t rows = 0, lastWifi = 0;
static char     lastSeen[40] = {0};

// avoid logging the same MAC twice a minute
#define SEEN_MAX 120
static uint8_t seen[SEEN_MAX][6];
static uint32_t seenAt[SEEN_MAX];
static int seenN = 0;

// BLE advert -> main loop queue; SD writes never happen in the BT callback.
struct Hit { uint8_t mac[6]; char name[24]; int8_t rssi; };
#define HITQ 12
static Hit hq[HITQ];
static volatile int hHead = 0, hTail = 0;

static bool recent(const uint8_t *mac) {
  uint32_t now = millis();
  for (int i = 0; i < seenN; i++)
    if (memcmp(seen[i], mac, 6) == 0) {
      if (now - seenAt[i] < 60000) return true;
      seenAt[i] = now; return false;
    }
  if (seenN < SEEN_MAX) { memcpy(seen[seenN], mac, 6); seenAt[seenN] = now; seenN++; }
  return false;
}

static void logRow(const uint8_t *mac, const char *label, int rssi, bool isBle, int chan) {
  if (!csv) return;
  char line[160];
  snprintf(line, sizeof(line), "%04u-%02u-%02u,%02u:%02u:%02u,%s,%02X:%02X:%02X:%02X:%02X:%02X,%d,%d,%s,\"%s\"\n",
           SYS.year, SYS.month, SYS.day, SYS.hh, SYS.mm, SYS.ss,
           isBle ? "BLE" : "WiFi",
           mac[0],mac[1],mac[2],mac[3],mac[4],mac[5],
           rssi, chan, oui_vendor(mac), label ? label : "");
  csv.print(line);
  rows++;
  snprintf(lastSeen, sizeof(lastSeen), "%s %s", oui_vendor(mac), label ? label : "");
}

class EvCb : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice d) override {
    int nh = (hHead + 1) % HITQ;
    if (nh == hTail) return;                 // queue full, drop
    uint8_t *m = d.getAddress().getNative();
    memcpy(hq[hHead].mac, m, 6);
    hq[hHead].rssi = d.getRSSI();
    String nm = d.haveName() ? d.getName() : String("");
    snprintf(hq[hHead].name, sizeof(hq[hHead].name), "%s", nm.c_str());
    hHead = nh;
  }
};
static EvCb cb;

void app_evidence_open(lv_obj_t *body) {
  rows = 0; seenN = 0; lastSeen[0] = 0; hHead = hTail = 0;

  statusLbl = lv_label_create(body);
  lv_obj_set_style_text_color(statusLbl, COL_TEXT_2, LV_PART_MAIN);
  lv_obj_set_style_text_font(statusLbl, &lv_font_montserrat_16, LV_PART_MAIN);

  SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0);
  sdOk = SD_MMC.begin("/sdcard", true, false);
  if (sdOk) {
    bool fresh = !SD_MMC.exists("/evidence.csv");
    csv = SD_MMC.open("/evidence.csv", FILE_APPEND);
    if (csv) { if (fresh) csv.print("date,time,radio,mac,rssi,chan,vendor,label\n"); csv.flush(); }
    else sdOk = false;
  }
  lv_label_set_text(statusLbl, sdOk ? "logging to /evidence.csv" : "no SD card - insert one and reopen");

  lv_obj_t *card = ui_card(body);
  countLbl = lv_label_create(card);
  lv_obj_set_style_text_color(countLbl, COL_TEXT, LV_PART_MAIN);
  lv_obj_set_style_text_font(countLbl, &lv_font_montserrat_40, LV_PART_MAIN);
  lv_label_set_text(countLbl, "0");
  lv_obj_t *cl = lv_label_create(card);
  lv_obj_set_style_text_color(cl, COL_TEXT_2, LV_PART_MAIN);
  lv_obj_set_style_text_font(cl, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_label_set_text(cl, "rows logged");
  lastLbl = lv_label_create(card);
  lv_obj_set_style_text_color(lastLbl, COL_TEXT_2, LV_PART_MAIN);
  lv_obj_set_style_text_font(lastLbl, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_label_set_text(lastLbl, "");

  lv_obj_t *nc = ui_card(body);
  note = lv_label_create(nc);
  lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(note, LV_PCT(100));
  lv_obj_set_style_text_color(note, COL_SEP, LV_PART_MAIN);
  lv_obj_set_style_text_font(note, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_label_set_text(note, "each MAC is logged once a minute with timestamp, RSSI, channel and "
                          "vendor. open the CSV in a spreadsheet later.");

  if (sdOk) {
    WiFi.mode(WIFI_STA); WiFi.disconnect(false, true);
    WiFi.scanNetworks(true, true); lastWifi = millis();
    ble_scan_start(&cb, true, 120, 100);
  }
}

void app_evidence_tick(void) {
  { static uint32_t _lg=0; uint32_t _n=millis();
    if(_n-_lg>1000){_lg=_n; USBSerial.printf("[evidence] sd=%d rows=%lu\n",(int)sdOk,(unsigned long)rows);} }
  if (!sdOk) return;
  while (hTail != hHead) {                    // drain BLE hits in loop context
    Hit &h = hq[hTail];
    if (!recent(h.mac)) logRow(h.mac, h.name, h.rssi, true, 0);
    hTail = (hTail + 1) % HITQ;
  }
  int wn = WiFi.scanComplete();
  if (wn >= 0) {
    for (int i = 0; i < wn; i++) {
      uint8_t *b = WiFi.BSSID(i);
      if (b && !recent(b)) {
        String ss = WiFi.SSID(i);
        logRow(b, ss.length()?ss.c_str():"(hidden)", WiFi.RSSI(i), false, WiFi.channel(i));
      }
    }
    WiFi.scanDelete(); WiFi.scanNetworks(true, true);
  }
  static uint32_t last = 0, lastFlush = 0;
  uint32_t now = millis();
  if (now - last > 500) { last = now;
    if (countLbl) lv_label_set_text_fmt(countLbl, "%u", rows);
    if (lastLbl)  lv_label_set_text_fmt(lastLbl, "last: %s", lastSeen);
  }
  if (csv && now - lastFlush > 3000) { lastFlush = now; csv.flush(); }
}

void app_evidence_close(void) {
  ble_scan_stop();   // stop scan, keep controller up
  // Leave WiFi UP (only free the scan results). Fully stopping WiFi here
  // (WiFi.mode(WIFI_OFF)) then bringing it back for the next app HANGS under
  // WiFi+BLE coexistence -- the exact freeze radio.cpp documents. Every other
  // app leaves WiFi up via radio_stop(); match that.
  WiFi.scanDelete();
  if (csv) { csv.flush(); csv.close(); }
  if (sdOk) SD_MMC.end();
  sdOk = false;
  statusLbl = countLbl = lastLbl = note = NULL;
}
