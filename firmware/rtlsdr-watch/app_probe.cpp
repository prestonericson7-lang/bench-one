// Probe sniffer: capture the SSIDs nearby phones are actively searching for.
// A phone's probe requests are a list of networks it remembers -- home, work,
// the coffee shop -- leaking out of every pocket. Receive-only.
#include "ui.h"
#include "radio.h"
#include "oui.h"

#define PROBE_MAX 40

struct Probe { char ssid[33]; uint8_t src[6]; int8_t rssi; uint16_t hits; };
static Probe          probes[PROBE_MAX];
static volatile int   pCount = 0;
static volatile bool  pDirty = false;
static portMUX_TYPE   pmux = portMUX_INITIALIZER_UNLOCKED;
static lv_obj_t *statusLbl = NULL, *listBox = NULL;

static void sniff(void *buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT) return;
  wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
  wifi_hdr_t *h = (wifi_hdr_t *)pkt->payload;
  if (FTYPE(h->fctl) != FT_MGMT || FSUBTYPE(h->fctl) != ST_PROBE_REQ) return;

  // tagged params start at byte 24; first tag should be SSID (id 0)
  const uint8_t *p = pkt->payload + 24;
  int len = pkt->rx_ctrl.sig_len - 24 - 4;   // minus FCS
  if (len < 2 || p[0] != 0) return;
  int slen = p[1];
  if (slen <= 0 || slen > 32 || 2 + slen > len) return;  // skip broadcast (len 0) + junk

  char ssid[33]; memcpy(ssid, p + 2, slen); ssid[slen] = 0;
  for (int i = 0; i < slen; i++) if (ssid[i] < 32 || ssid[i] > 126) return;  // printable only

  portENTER_CRITICAL(&pmux);
  int found = -1;
  for (int i = 0; i < pCount; i++) if (!strcmp(probes[i].ssid, ssid)) { found = i; break; }
  if (found < 0 && pCount < PROBE_MAX) {
    found = pCount++;
    snprintf(probes[found].ssid, sizeof(probes[found].ssid), "%s", ssid);
    probes[found].hits = 0;
  }
  if (found >= 0) {
    memcpy(probes[found].src, h->addr2, 6);
    probes[found].rssi = pkt->rx_ctrl.rssi;
    probes[found].hits++;
    pDirty = true;
  }
  portEXIT_CRITICAL(&pmux);
}

void app_probe_open(lv_obj_t *body) {
  pCount = 0; pDirty = false;
  statusLbl = lv_label_create(body);
  lv_obj_set_style_text_color(statusLbl, COL_TEXT_2, LV_PART_MAIN);
  lv_obj_set_style_text_font(statusLbl, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_label_set_text(statusLbl, "Listening for probe requests...");

  listBox = lv_obj_create(body);
  lv_obj_set_width(listBox, LV_PCT(100));
  lv_obj_set_height(listBox, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(listBox, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(listBox, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(listBox, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_row(listBox, 8, LV_PART_MAIN);
  lv_obj_set_flex_flow(listBox, LV_FLEX_FLOW_COLUMN);
  lv_obj_remove_flag(listBox, LV_OBJ_FLAG_SCROLLABLE);

  radio_start(sniff, true);   // hop channels to catch probes anywhere
}

void app_probe_tick(void) {
  radio_tick();
  { static uint32_t _lg=0; uint32_t _n=millis();
    if(_n-_lg>1000){_lg=_n; USBSerial.printf("[probe] ch%d ssids=%d\n",radio_channel(),pCount);} }
  static uint32_t last = 0;
  if (!pDirty || millis() - last < 800) return;
  last = millis();

  Probe snap[PROBE_MAX]; int n;
  portENTER_CRITICAL(&pmux);
  n = pCount; memcpy(snap, probes, sizeof(Probe) * (n > PROBE_MAX ? PROBE_MAX : n));
  pDirty = false;
  portEXIT_CRITICAL(&pmux);

  for (int i = 1; i < n; i++) { Probe k = snap[i]; int j = i-1;
    while (j >= 0 && snap[j].hits < k.hits) { snap[j+1] = snap[j]; j--; } snap[j+1] = k; }

  lv_obj_clean(listBox);
  if (statusLbl) lv_label_set_text_fmt(statusLbl, "%d network%s sought · ch %d",
                                       n, n==1?"":"s", radio_channel());
  for (int i = 0; i < n; i++) {
    char sub[64];
    snprintf(sub, sizeof(sub), "from %s · %dx · %ddBm",
             oui_vendor(snap[i].src), snap[i].hits, snap[i].rssi);
    ui_row(listBox, snap[i].ssid, sub);
  }
}

void app_probe_close(void) {
  radio_stop();
  statusLbl = NULL; listBox = NULL; pCount = 0;
}
