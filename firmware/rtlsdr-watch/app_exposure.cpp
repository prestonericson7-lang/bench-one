// Exposure: one number for "how many trackable radios are on you right now."
// Counts unique Wi-Fi transmitters (promiscuous) and unique BLE advertisers
// (concurrent scan), deduped by MAC. Pure RX -- it only listens.
#include "ui.h"
#include "oui.h"
#include "radio.h"
#include "ble.h"

#define CAP 160
static uint8_t  wifiMac[CAP][6]; static volatile int nWifi = 0;
static uint8_t  bleMac[CAP][6];  static volatile int nBle  = 0;
static volatile int nPriv = 0;                 // randomized (trackable-but-hiding) count
static portMUX_TYPE emux = portMUX_INITIALIZER_UNLOCKED;

static bool seen_add(uint8_t table[][6], volatile int *n, const uint8_t *m) {
  for (int i = 0; i < *n; i++) if (!memcmp(table[i], m, 6)) return false;
  if (*n < CAP) { memcpy(table[*n], m, 6); (*n)++; if (m[0] & 0x02) nPriv++; return true; }
  return false;
}
static bool mcast(const uint8_t *m) { return m[0] & 0x01; }

static void sniff(void *buf, wifi_promiscuous_pkt_type_t type) {
  LV_UNUSED(type);
  wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
  wifi_hdr_t *h = (wifi_hdr_t *)pkt->payload;
  if (mcast(h->addr2)) return;
  portENTER_CRITICAL(&emux); seen_add(wifiMac, &nWifi, h->addr2); portEXIT_CRITICAL(&emux);
}

class ExpCb : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice d) override {
    uint8_t *m = d.getAddress().getNative();
    portENTER_CRITICAL(&emux); seen_add(bleMac, &nBle, m); portEXIT_CRITICAL(&emux);
  }
};
static ExpCb cb;

static lv_obj_t *bigNum = NULL, *wordLbl = NULL, *breakdown = NULL, *note = NULL;

void app_exposure_open(lv_obj_t *body) {
  nWifi = nBle = nPriv = 0;

  bigNum = lv_label_create(body);
  lv_obj_set_width(bigNum, LV_PCT(100));
  lv_obj_set_style_text_align(bigNum, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_style_text_color(bigNum, COL_TEXT, LV_PART_MAIN);
  lv_obj_set_style_text_font(bigNum, &lv_font_montserrat_40, LV_PART_MAIN);
  lv_label_set_text(bigNum, "0");

  wordLbl = lv_label_create(body);
  lv_obj_set_width(wordLbl, LV_PCT(100));
  lv_obj_set_style_text_align(wordLbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_style_text_color(wordLbl, COL_GREEN, LV_PART_MAIN);
  lv_obj_set_style_text_font(wordLbl, &lv_font_montserrat_22, LV_PART_MAIN);
  lv_label_set_text(wordLbl, "SCANNING");

  lv_obj_t *card = ui_card(body);
  breakdown = lv_label_create(card);
  lv_obj_set_style_text_color(breakdown, COL_TEXT, LV_PART_MAIN);
  lv_obj_set_style_text_font(breakdown, &lv_font_montserrat_18, LV_PART_MAIN);
  lv_label_set_text(breakdown, "");

  lv_obj_t *nc = ui_card(body);
  note = lv_label_create(nc);
  lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(note, LV_PCT(100));
  lv_obj_set_style_text_color(note, COL_SEP, LV_PART_MAIN);
  lv_obj_set_style_text_font(note, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_label_set_text(note, "unique radios heard since you opened this. randomized MACs are "
                          "counted too -- a device hiding its address is still a device.");

  radio_start(sniff, true);
  ble_scan_start(&cb, true, 120, 100);
}

void app_exposure_tick(void) {
  radio_tick();
  static uint32_t last = 0;
  uint32_t now = millis();
  if (now - last < 700) return;
  last = now;

  int w, b, p;
  portENTER_CRITICAL(&emux); w = nWifi; b = nBle; p = nPriv; portEXIT_CRITICAL(&emux);
  int total = w + b;
  lv_label_set_text_fmt(bigNum, "%d", total);
  lv_label_set_text_fmt(breakdown, "%d Wi-Fi   ·   %d BLE\n%d randomized", w, b, p);

  const char *s; lv_color_t c;
  if      (total < 8)  { s = "QUIET";    c = COL_GREEN;  }
  else if (total < 20) { s = "MODERATE"; c = COL_YELLOW; }
  else if (total < 45) { s = "BUSY";     c = COL_ORANGE; }
  else                 { s = "CROWDED";  c = COL_RED;    }
  lv_label_set_text(wordLbl, s);
  lv_obj_set_style_text_color(wordLbl, c, LV_PART_MAIN);
}

void app_exposure_close(void) {
  radio_stop();
  ble_scan_stop();
  bigNum = wordLbl = breakdown = note = NULL;
}
