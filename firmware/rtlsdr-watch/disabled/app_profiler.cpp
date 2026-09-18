// Profiler: a real 802.11 correlation tool, not a surface scan.
//
// Runs the radio in PROMISCUOUS mode + a concurrent BLE scan (both stay up the
// whole time -- no on/off cycling, which was what rebooted the board). From the
// live frames it builds relationships instead of a flat list:
//   - APs (from beacons / probe responses), with SSID + vendor
//   - each CLIENT tied to the AP it's actually talking to (data-frame ToDS/FromDS)
//   - the SSIDs a client is probing for = its network history ("what wifi it's on")
//   - a guess at what each device IS (phone / computer / etc) from its OUI
// BLE is shown as its own layer because a phone randomises its WiFi and BLE MACs
// independently, so the two genuinely can't be tied together -- we don't fake it.
//
// Crash fixes vs the old version:
//   * the row list is NEVER rebuilt while the user is touching or the list is
//     still gliding -- deleting a row out from under an active scroll is what
//     reset the ESP32. Scroll position is preserved across refreshes.
//   * WiFi and BLE are started once and left up; the old 9s/3s teardown cycle is
//     gone.
#include "ui.h"
#include "oui.h"
#include "radio.h"
#include "ble.h"

#define MAX_AP   24
#define MAX_STA  48
#define MAX_BLE  24
#define MAX_SHOW 40          // cap objects created per refresh (bounds RAM + time)
static const uint8_t ZERO6[6] = {0};

struct AP  { uint8_t bssid[6]; char ssid[24]; uint8_t ch; int8_t rssi; uint32_t seen; };
struct STA { uint8_t mac[6];  uint8_t bssid[6]; char probe[24]; int8_t rssi; uint32_t seen; };
struct BL  { uint8_t mac[6];  char name[24]; int8_t rssi; };

static AP  aps[MAX_AP];   static volatile int nAP = 0;
static STA stas[MAX_STA]; static volatile int nSTA = 0;
static BL  bls[MAX_BLE];  static volatile int nBLE = 0;
static volatile bool     dirty  = false;
static volatile uint32_t rxCount = 0;
static portMUX_TYPE      mux = portMUX_INITIALIZER_UNLOCKED;

static lv_obj_t *statusLbl = NULL, *listBox = NULL, *scroller = NULL;
static volatile bool scrolling = false;

// ---- table helpers (all called under mux) ----
static int ap_idx(const uint8_t *b) {
  for (int i = 0; i < nAP; i++) if (!memcmp(aps[i].bssid, b, 6)) return i;
  if (nAP < MAX_AP) { int i = nAP++; memcpy(aps[i].bssid, b, 6); aps[i].ssid[0]=0; return i; }
  return -1;
}
static int sta_idx(const uint8_t *m) {
  for (int i = 0; i < nSTA; i++) if (!memcmp(stas[i].mac, m, 6)) return i;
  if (nSTA < MAX_STA) { int i = nSTA++; memcpy(stas[i].mac, m, 6);
                        memset(stas[i].bssid,0,6); stas[i].probe[0]=0; return i; }
  return -1;
}
static bool mcast(const uint8_t *m){ return (m[0] & 0x01) || !memcmp(m, ZERO6, 6); }

// pull the SSID (tag id 0) out of tagged params starting at `off`
static void ssid_from(const uint8_t *p, int len, int off, char *dst, int dn) {
  dst[0] = 0;
  while (off + 2 <= len) {
    uint8_t id = p[off], l = p[off+1];
    if (off + 2 + l > len) break;
    if (id == 0) { char raw[33]; int n = l>32?32:l; memcpy(raw,p+off+2,n); raw[n]=0;
                   ui_safe(dst, dn, raw); return; }
    off += 2 + l;
  }
}

static void sniff(void *buf, wifi_promiscuous_pkt_type_t type) {
  wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
  wifi_hdr_t *h = (wifi_hdr_t *)pkt->payload;
  int len = pkt->rx_ctrl.sig_len;
  int8_t rssi = pkt->rx_ctrl.rssi;
  uint8_t ch = pkt->rx_ctrl.channel;
  uint16_t fc = h->fctl;
  uint8_t ft = FTYPE(fc), st = FSUBTYPE(fc);
  bool tods = fc & 0x0100, fromds = fc & 0x0200;
  uint32_t now = millis();
  rxCount++;

  // City-proof: in a dense RF environment data frames flood in by the thousand.
  // Sample them (1 in 8) so this callback can't hog the CPU and trip the watchdog
  // (that was the freeze). Management frames (beacons/probes) are rarer -> kept whole.
  if (ft == FT_DATA) { static uint32_t dcnt = 0; if ((dcnt++ & 7) != 0) return; }

  portENTER_CRITICAL(&mux);
  if (ft == FT_MGMT && (st == ST_BEACON || st == ST_PROBE_RESP)) {
    int i = ap_idx(h->addr3);
    if (i >= 0) { aps[i].rssi = rssi; aps[i].ch = ch; aps[i].seen = now;
                  if (len > 36) ssid_from(pkt->payload, len, 36, aps[i].ssid, sizeof(aps[i].ssid)); dirty = true; }
  } else if (ft == FT_MGMT && st == ST_PROBE_REQ) {
    if (!mcast(h->addr2)) {
      int i = sta_idx(h->addr2);
      if (i >= 0) { stas[i].rssi = rssi; stas[i].seen = now;
                    if (len > 24) ssid_from(pkt->payload, len, 24, stas[i].probe, sizeof(stas[i].probe)); dirty = true; }
    }
  } else if (ft == FT_DATA) {
    const uint8_t *sta = NULL, *bss = NULL;
    if (tods && !fromds)      { bss = h->addr1; sta = h->addr2; }
    else if (!tods && fromds) { bss = h->addr2; sta = h->addr1; }
    if (sta && !mcast(sta) && !mcast(bss)) {
      int i = sta_idx(sta);
      if (i >= 0) { memcpy(stas[i].bssid, bss, 6); stas[i].rssi = rssi; stas[i].seen = now; dirty = true; }
      ap_idx(bss);   // ensure the AP exists even if we haven't heard its beacon yet
    }
  }
  portEXIT_CRITICAL(&mux);
}

class PBCb : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice d) override {
    uint8_t *m = d.getAddress().getNative();
    char nm[24] = {0};
    if (d.haveName()) ui_safe(nm, sizeof(nm), d.getName().c_str());
    int r = d.getRSSI();
    portENTER_CRITICAL(&mux);
    int f = -1;
    for (int i = 0; i < nBLE; i++) if (!memcmp(bls[i].mac, m, 6)) { f = i; break; }
    if (f < 0 && nBLE < MAX_BLE) { f = nBLE++; memcpy(bls[f].mac, m, 6); bls[f].name[0]=0; }
    if (f >= 0) { bls[f].rssi = r; if (nm[0]) memcpy(bls[f].name, nm, sizeof(nm)); dirty = true; }
    portEXIT_CRITICAL(&mux);
  }
};
static PBCb cb;

static lv_color_t rcol(int r){ return r>=-55?COL_GREEN : r>=-72?COL_YELLOW : COL_ORANGE; }

// a row can carry a device we can hand to Tail
struct Tap { uint8_t mac[6]; char nm[24]; bool ble; };
static Tap taps[MAX_SHOW];
static int nTap = 0;
static int rowCount = 0;   // rows emitted this rebuild

static void tail_row(lv_event_t *e) {
  int t = (int)(intptr_t)lv_event_get_user_data(e);
  if (t < 0 || t >= nTap) return;
  buzz(15);
  tail_set_target(taps[t].mac, taps[t].nm, taps[t].ble);
  ui_open_app_by_name("Tail");
}

static void addrow(const char *title, const char *sub, lv_color_t pipc, const char *pip,
                   const uint8_t *mac, const char *nm, bool ble, int indent) {
  if (rowCount >= MAX_SHOW) return;
  lv_obj_t *row = ui_row(listBox, title, sub);
  if (indent) lv_obj_set_style_pad_left(row, 26, LV_PART_MAIN);
  if (mac) {
    int t = rowCount;
    memcpy(taps[t].mac, mac, 6); snprintf(taps[t].nm, sizeof(taps[t].nm), "%s", nm); taps[t].ble = ble;
    nTap = t + 1;
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, tail_row, LV_EVENT_CLICKED, (void*)(intptr_t)t);
  }
  if (pip) {
    lv_obj_t *p = lv_label_create(row);
    lv_label_set_text(p, pip);
    lv_obj_align(p, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_set_style_text_color(p, pipc, LV_PART_MAIN);
    lv_obj_set_style_text_font(p, &lv_font_montserrat_16, LV_PART_MAIN);
  }
  rowCount++;
}

// scroll guards: never delete rows while a touch or a fling is in progress.
static void scroll_begin(lv_event_t *e){ LV_UNUSED(e); scrolling = true;  }
static void scroll_end(lv_event_t *e)  { LV_UNUSED(e); scrolling = false; }

static void rebuild(void) {
  if (!listBox) return;
  AP a[MAX_AP]; STA s[MAX_STA]; BL b[MAX_BLE]; int na, ns, nb;
  portENTER_CRITICAL(&mux);
  na=nAP; ns=nSTA; nb=nBLE;
  memcpy(a,aps,sizeof(AP)*na); memcpy(s,stas,sizeof(STA)*ns); memcpy(b,bls,sizeof(BL)*nb);
  dirty=false;
  portEXIT_CRITICAL(&mux);

  int sy = scroller ? lv_obj_get_scroll_y(scroller) : 0;   // keep the reader's place
  lv_obj_clean(listBox);
  rowCount = 0; nTap = 0;

  if (statusLbl) lv_label_set_text_fmt(statusLbl, "%d nets  ·  %d clients  ·  %d BLE  ·  ch%d",
                                       na, ns, nb, radio_channel());

  // --- APs, each followed by its associated clients ---
  for (int i = 0; i < na && rowCount < MAX_SHOW; i++) {
    char title[26], sub[80];
    if (a[i].ssid[0]) snprintf(title,sizeof(title),"%s",a[i].ssid);
    else snprintf(title,sizeof(title),"%02X:%02X:%02X:%02X:%02X:%02X",
                  a[i].bssid[0],a[i].bssid[1],a[i].bssid[2],a[i].bssid[3],a[i].bssid[4],a[i].bssid[5]);
    int clients = 0;
    for (int j = 0; j < ns; j++) if (!memcmp(s[j].bssid, a[i].bssid, 6)) clients++;
    snprintf(sub,sizeof(sub),"AP · %s · ch%d · %ddBm · %d client%s",
             oui_vendor(a[i].bssid), a[i].ch, a[i].rssi, clients, clients==1?"":"s");
    addrow(title, sub, rcol(a[i].rssi), LV_SYMBOL_WIFI, a[i].bssid,
           a[i].ssid[0]?a[i].ssid:oui_vendor(a[i].bssid), false, 0);

    int shown = 0;
    for (int j = 0; j < ns && shown < 8 && rowCount < MAX_SHOW; j++) {
      if (memcmp(s[j].bssid, a[i].bssid, 6)) continue;
      shown++;
      char ct[26], cs[72];
      snprintf(ct,sizeof(ct),"%02X:%02X:%02X:%02X:%02X:%02X",
               s[j].mac[0],s[j].mac[1],s[j].mac[2],s[j].mac[3],s[j].mac[4],s[j].mac[5]);
      snprintf(cs,sizeof(cs),"%s · %s · %ddBm",
               device_kind(oui_vendor(s[j].mac), false), oui_vendor(s[j].mac), s[j].rssi);
      addrow(ct, cs, rcol(s[j].rssi), NULL, s[j].mac, oui_vendor(s[j].mac), false, 1);
    }
  }

  // --- clients not tied to any seen AP (searching), with their network history ---
  bool hdr = false;
  for (int j = 0; j < ns && rowCount < MAX_SHOW; j++) {
    if (memcmp(s[j].bssid, ZERO6, 6)) continue;             // already shown under an AP
    if (!hdr) { addrow("UNASSOCIATED", "devices searching for a network", COL_TEXT_2, NULL, NULL, NULL, false, 0); hdr = true; }
    char ct[26], cs[80];
    snprintf(ct,sizeof(ct),"%02X:%02X:%02X:%02X:%02X:%02X",
             s[j].mac[0],s[j].mac[1],s[j].mac[2],s[j].mac[3],s[j].mac[4],s[j].mac[5]);
    if (s[j].probe[0]) snprintf(cs,sizeof(cs),"%s · looking for \"%s\" · %ddBm",
                                device_kind(oui_vendor(s[j].mac), false), s[j].probe, s[j].rssi);
    else               snprintf(cs,sizeof(cs),"%s · %s · %ddBm",
                                device_kind(oui_vendor(s[j].mac), false), oui_vendor(s[j].mac), s[j].rssi);
    addrow(ct, cs, rcol(s[j].rssi), NULL, s[j].mac, oui_vendor(s[j].mac), false, 1);
  }

  // --- BLE layer ---
  if (nb && rowCount < MAX_SHOW)
    addrow("BLE LAYER", "separate radio · can't be tied to WiFi (MAC randomised)", COL_TEXT_2, NULL, NULL, NULL, false, 0);
  for (int i = 0; i < nb && rowCount < MAX_SHOW; i++) {
    char ct[26], cs[72];
    snprintf(ct,sizeof(ct),"%s", b[i].name[0]?b[i].name:"(unnamed)");
    snprintf(cs,sizeof(cs),"BLE · %s · %s · %ddBm", device_kind(oui_vendor(b[i].mac), true),
             oui_vendor(b[i].mac), b[i].rssi);
    addrow(ct, cs, rcol(b[i].rssi), LV_SYMBOL_BLUETOOTH, b[i].mac,
           b[i].name[0]?b[i].name:oui_vendor(b[i].mac), true, 1);
  }

  if (scroller) {
    lv_obj_update_layout(listBox);
    lv_obj_scroll_to_y(scroller, sy, LV_ANIM_OFF);          // restore place
  }
}

void app_profiler_open(lv_obj_t *body) {
  nAP=0; nSTA=0; nBLE=0; dirty=false; rxCount=0; scrolling=false;
  scroller = body;
  lv_obj_add_event_cb(body, scroll_begin, LV_EVENT_SCROLL_BEGIN, NULL);
  lv_obj_add_event_cb(body, scroll_end,   LV_EVENT_SCROLL_END,   NULL);

  statusLbl = lv_label_create(body);
  lv_obj_set_style_text_color(statusLbl, COL_TEXT_2, LV_PART_MAIN);
  lv_obj_set_style_text_font(statusLbl, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_label_set_text(statusLbl, "sniffing 802.11 + BLE...");

  listBox = lv_obj_create(body);
  lv_obj_set_width(listBox, LV_PCT(100));
  lv_obj_set_height(listBox, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(listBox, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(listBox, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(listBox, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_row(listBox, 7, LV_PART_MAIN);
  lv_obj_set_flex_flow(listBox, LV_FLEX_FLOW_COLUMN);
  lv_obj_remove_flag(listBox, LV_OBJ_FLAG_SCROLLABLE);

  radio_start(sniff, true);                    // promiscuous + channel hop (continuous)
  ble_scan_start(&cb, true, 120, 100);         // concurrent BLE scan (controller stays up)
  USBSerial.println("[profiler] started (wifi promisc + ble)");
}

void app_profiler_tick(void) {
  radio_tick();                                // channel hop
  uint32_t now = millis();

  static uint32_t lastLog = 0;
  if (now - lastLog > 1000) {
    lastLog = now;
    USBSerial.printf("[profiler] ch%d rx=%lu ap=%d sta=%d ble=%d heap=%u\n",
                     radio_channel(), (unsigned long)rxCount, nAP, nSTA, nBLE,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  }

  static uint32_t last = 0;
  if (dirty && now - last > 3000 && !scrolling && !g_ui_touching) { last = now; rebuild(); }
}

void app_profiler_close(void) {
  radio_stop();
  ble_scan_stop();                             // stop scan, keep controller alive
  statusLbl = NULL; listBox = NULL; scroller = NULL;
  nAP=nSTA=nBLE=0;
}
