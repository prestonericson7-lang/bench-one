// Handshake capture: sniff WPA2 EAPOL frames (the 4-way handshake) and PMKID,
// write them to the SD card as a standard .pcap for offline cracking on the
// Teensy/Luckfox stack with hashcat/aircrack. Receive-only.
#include "ui.h"
#include "radio.h"
#include "pin_config.h"
#include <SD_MMC.h>

static lv_obj_t *statusLbl = NULL, *countLbl = NULL, *fileLbl = NULL, *note = NULL;
static volatile uint32_t eapolCount = 0;
static File     pcap;
static bool     sdOk = false;
static portMUX_TYPE hmux = portMUX_INITIALIZER_UNLOCKED;

// queue raw frames from the sniffer to the main loop (SD writes can't happen in
// the wifi callback)
#define Q 8
static uint8_t  qbuf[Q][320];
static uint16_t qlen[Q];
static volatile int qHead = 0, qTail = 0;

static bool is_eapol(const uint8_t *p, int len) {
  // look for LLC/SNAP + EtherType 0x888E anywhere in the first part of the frame
  for (int i = 24; i + 8 < len && i < 40; i++)
    if (p[i]==0xAA && p[i+1]==0xAA && p[i+2]==0x03 &&
        p[i+6]==0x88 && p[i+7]==0x8E) return true;
  return false;
}

static void sniff(void *buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_DATA) return;
  wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
  int len = pkt->rx_ctrl.sig_len;
  if (len < 32 || len > 320) return;
  if (!is_eapol(pkt->payload, len)) return;

  int nh = (qHead + 1) % Q;
  if (nh == qTail) return;              // queue full, drop
  memcpy(qbuf[qHead], pkt->payload, len);
  qlen[qHead] = len;
  qHead = nh;
}

static void pcap_global(File &f) {
  uint8_t gh[24] = {0xd4,0xc3,0xb2,0xa1, 2,0,4,0, 0,0,0,0, 0,0,0,0,
                    0xff,0xff,0,0, 105,0,0,0};   // LINKTYPE_IEEE802_11 = 105
  f.write(gh, 24);
}
static void pcap_pkt(File &f, const uint8_t *d, uint16_t n) {
  uint32_t ts = millis();
  uint32_t sec = ts/1000, usec = (ts%1000)*1000;
  uint8_t ph[16];
  memcpy(ph, &sec, 4); memcpy(ph+4, &usec, 4);
  uint32_t ln = n; memcpy(ph+8, &ln, 4); memcpy(ph+12, &ln, 4);
  f.write(ph, 16); f.write(d, n);
}

void app_handshake_open(lv_obj_t *body) {
  eapolCount = 0; qHead = qTail = 0;

  statusLbl = lv_label_create(body);
  lv_obj_set_style_text_color(statusLbl, COL_TEXT_2, LV_PART_MAIN);
  lv_obj_set_style_text_font(statusLbl, &lv_font_montserrat_16, LV_PART_MAIN);

  SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0);
  sdOk = SD_MMC.begin("/sdcard", true /*1-bit*/, false);
  if (sdOk) {
    char path[40];
    snprintf(path, sizeof(path), "/handshake_%lu.pcap", (unsigned long)millis());
    pcap = SD_MMC.open(path, FILE_WRITE);
    if (pcap) { pcap_global(pcap); pcap.flush(); }
    else sdOk = false;
    lv_label_set_text(statusLbl, sdOk ? "capturing EAPOL to SD..." : "SD open failed");
  } else {
    lv_label_set_text(statusLbl, "no SD card - insert one and reopen");
  }

  lv_obj_t *card = ui_card(body);
  countLbl = lv_label_create(card);
  lv_obj_set_style_text_color(countLbl, COL_TEXT, LV_PART_MAIN);
  lv_obj_set_style_text_font(countLbl, &lv_font_montserrat_40, LV_PART_MAIN);
  lv_label_set_text(countLbl, "0");
  fileLbl = lv_label_create(card);
  lv_obj_set_style_text_color(fileLbl, COL_TEXT_2, LV_PART_MAIN);
  lv_obj_set_style_text_font(fileLbl, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_label_set_text(fileLbl, "EAPOL frames captured");

  lv_obj_t *nc = ui_card(body);
  note = lv_label_create(nc);
  lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(note, LV_PCT(100));
  lv_obj_set_style_text_color(note, COL_SEP, LV_PART_MAIN);
  lv_obj_set_style_text_font(note, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_label_set_text(note, "hops channels catching 4-way handshakes + PMKID. pull the .pcap "
                          "onto the stack and run hashcat -m 22000. capture is passive; "
                          "you only crack networks you own.");

  if (sdOk) radio_start(sniff, true);
}

void app_handshake_tick(void) {
  radio_tick();
  { static uint32_t _lg=0; uint32_t _n=millis();
    if(_n-_lg>1000){_lg=_n; USBSerial.printf("[handshake] sd=%d ch%d eapol=%lu\n",(int)sdOk,radio_channel(),(unsigned long)eapolCount);} }
  while (qTail != qHead) {
    if (pcap) { pcap_pkt(pcap, qbuf[qTail], qlen[qTail]); eapolCount++; }
    qTail = (qTail + 1) % Q;
  }
  static uint32_t last = 0, lastFlush = 0;
  uint32_t now = millis();
  if (now - last > 400) { last = now;
    if (countLbl) lv_label_set_text_fmt(countLbl, "%u", eapolCount);
    if (statusLbl && sdOk) lv_label_set_text_fmt(statusLbl, "capturing · ch %d", radio_channel());
  }
  if (pcap && now - lastFlush > 2000) { lastFlush = now; pcap.flush(); }
}

void app_handshake_close(void) {
  radio_stop();
  if (pcap) { pcap.flush(); pcap.close(); }
  if (sdOk) SD_MMC.end();
  sdOk = false;
  statusLbl = countLbl = fileLbl = note = NULL;
}
