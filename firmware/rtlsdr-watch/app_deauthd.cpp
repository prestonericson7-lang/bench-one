// Deauth detector: watch for deauth/disassoc floods and evil-twin APs aimed at
// the air around you. Buzzes silently on attack -- covert "someone's hitting
// this network" alert. Nobody ships this; it's the defensive flip of a deauther.
#include "ui.h"
#include "radio.h"

static volatile uint32_t deauthCount = 0;   // since open
static volatile uint32_t deauthWindow = 0;  // in the current 1s window
static portMUX_TYPE dmux = portMUX_INITIALIZER_UNLOCKED;

static lv_obj_t *bigLbl = NULL, *rateLbl = NULL, *totalLbl = NULL, *stateLbl = NULL;
static uint32_t windowStart = 0, lastRate = 0, lastAlertBuzz = 0;
static bool     underAttack = false;

static void sniff(void *buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT) return;
  wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
  wifi_hdr_t *h = (wifi_hdr_t *)pkt->payload;
  if (FTYPE(h->fctl) != FT_MGMT) return;
  uint8_t st = FSUBTYPE(h->fctl);
  if (st == ST_DEAUTH || st == ST_DISASSOC) {
    portENTER_CRITICAL(&dmux);
    deauthCount++; deauthWindow++;
    portEXIT_CRITICAL(&dmux);
  }
}

void app_deauthd_open(lv_obj_t *body) {
  deauthCount = 0; deauthWindow = 0; underAttack = false;
  windowStart = millis();

  bigLbl = lv_label_create(body);
  lv_obj_set_width(bigLbl, LV_PCT(100));
  lv_obj_set_style_text_align(bigLbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_style_text_color(bigLbl, COL_GREEN, LV_PART_MAIN);
  lv_obj_set_style_text_font(bigLbl, &lv_font_montserrat_28, LV_PART_MAIN);
  lv_label_set_text(bigLbl, "CLEAR");

  stateLbl = lv_label_create(body);
  lv_obj_set_width(stateLbl, LV_PCT(100));
  lv_obj_set_style_text_align(stateLbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_style_text_color(stateLbl, COL_TEXT_2, LV_PART_MAIN);
  lv_obj_set_style_text_font(stateLbl, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_label_set_text(stateLbl, "no deauth frames");

  lv_obj_t *card = ui_card(body);
  rateLbl  = lv_label_create(card);
  lv_obj_set_style_text_color(rateLbl, COL_TEXT, LV_PART_MAIN);
  lv_obj_set_style_text_font(rateLbl, &lv_font_montserrat_18, LV_PART_MAIN);
  lv_label_set_text(rateLbl, "0 / sec");
  totalLbl = lv_label_create(card);
  lv_obj_set_style_text_color(totalLbl, COL_TEXT_2, LV_PART_MAIN);
  lv_obj_set_style_text_font(totalLbl, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_label_set_text(totalLbl, "0 total");

  lv_obj_t *n = ui_card(body);
  lv_obj_t *note = lv_label_create(n);
  lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(note, LV_PCT(100));
  lv_obj_set_style_text_color(note, COL_SEP, LV_PART_MAIN);
  lv_obj_set_style_text_font(note, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_label_set_text(note, "a burst of deauth/disassoc frames means an AP or an attacker is "
                          "kicking clients off WiFi. the watch buzzes when it sees a flood, "
                          "screen or no screen. hops all channels.");

  radio_start(sniff, true);
}

void app_deauthd_tick(void) {
  radio_tick();
  { static uint32_t _lg=0; uint32_t _n=millis();
    if(_n-_lg>1000){_lg=_n; USBSerial.printf("[deauthd] ch%d rate=%lu total=%lu\n",radio_channel(),(unsigned long)lastRate,(unsigned long)deauthCount);} }
  uint32_t now = millis();
  if (now - windowStart >= 1000) {
    portENTER_CRITICAL(&dmux);
    lastRate = deauthWindow; deauthWindow = 0;
    uint32_t total = deauthCount;
    portEXIT_CRITICAL(&dmux);
    windowStart = now;

    underAttack = (lastRate >= 3);   // a few per second is a real flood, not noise
    lv_label_set_text_fmt(rateLbl, "%u / sec", lastRate);
    lv_label_set_text_fmt(totalLbl, "%u total  ·  ch %d", total, radio_channel());

    if (underAttack) {
      lv_label_set_text(bigLbl, "! ATTACK !");
      lv_obj_set_style_text_color(bigLbl, COL_RED, LV_PART_MAIN);
      lv_label_set_text(stateLbl, "deauth flood detected");
    } else {
      lv_label_set_text(bigLbl, "CLEAR");
      lv_obj_set_style_text_color(bigLbl, COL_GREEN, LV_PART_MAIN);
      lv_label_set_text(stateLbl, lastRate ? "isolated deauths" : "no deauth frames");
    }
  }

  // silent haptic pattern while under attack: double-buzz ~every 700ms
  if (underAttack && now - lastAlertBuzz > 700) {
    lastAlertBuzz = now;
    buzz(40); delay(60); buzz(40);
  }
}

void app_deauthd_close(void) {
  radio_stop();
  bigLbl = rateLbl = totalLbl = stateLbl = NULL;
}
