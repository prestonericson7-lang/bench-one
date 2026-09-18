// Tail: pick a signal, then track it. Standalone now (the Profiler that used to
// feed it targets was removed).
//
//   PICK  - scan WiFi (APs + clients) and BLE, show a live list strongest-first,
//           tap one to lock it.
//   TRACK - big RSSI ring, motor buzzes faster as you close in (eyes-free), the
//           channel locks to the target so updates come several times a second.
//
// tail_set_target() is kept as an EXTERNAL hook: the E32R40T -> Teensy -> SDR chain
// (or anything else) can command Tail to jump straight to tracking a MAC.
#include "ui.h"
#include "radio.h"
#include "ble.h"
#include "oui.h"

uint8_t g_target_mac[6];
char    g_target_name[28] = {0};
bool    g_target_ble = false;
bool    g_target_set = false;

void tail_set_target(const uint8_t *mac, const char *name, bool isBle) {
  memcpy(g_target_mac, mac, 6);
  snprintf(g_target_name, sizeof(g_target_name), "%s", name ? name : "");
  g_target_ble = isBle;
  g_target_set = true;
}

// ---------------------------------------------------------------- state
enum Mode { PICK, TRACK };
static Mode      mode = PICK;
static int       pendingMode = -1;     // set by an event cb, applied in tick (never clean inside an event)
static lv_obj_t *bodyRef = NULL;

// pick list
#define MAX_SIG 48
#define SHOW_SIG 30
struct Sig { uint8_t mac[6]; char label[26]; int8_t rssi; bool ble; uint8_t kind; }; // kind 0=AP 1=client 2=BLE
static Sig            sigs[MAX_SIG];
static volatile int   nSig = 0;
static volatile bool  sDirty = false;
static portMUX_TYPE   smux = portMUX_INITIALIZER_UNLOCKED;
static lv_obj_t      *statusLbl = NULL, *listBox = NULL;
static volatile bool  scrolling = false;

// row -> which signal (captured at rebuild time so a tap knows the mac)
static uint8_t rowMac[SHOW_SIG][6];
static char    rowLabel[SHOW_SIG][26];
static bool    rowBle[SHOW_SIG];
static int     nRow = 0;

// track state
static lv_obj_t *arc = NULL, *lblRssi = NULL, *lblName = NULL, *lblHint = NULL;
static volatile int      curRssi = -100;
static volatile bool     haveRssi = false;
static volatile uint8_t  seenChan = 0;
static volatile uint32_t seenAt = 0;
static portMUX_TYPE      tmux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t          lastSeen = 0, lastBuzz = 0;

static int  prox_pct(int rssi) { int p = map(rssi, -95, -30, 0, 100); return p<0?0:(p>100?100:p); }
static lv_color_t rcol(int r){ return r>=-55?COL_GREEN : r>=-72?COL_YELLOW : COL_ORANGE; }
static bool mcast(const uint8_t *m){ return (m[0] & 0x01); }

// ---------------------------------------------------------------- PICK: collectors
static void sig_upsert(const uint8_t *mac, const char *label, int8_t rssi, bool ble, uint8_t kind) {
  portENTER_CRITICAL(&smux);
  int f = -1;
  for (int i = 0; i < nSig; i++) if (!memcmp(sigs[i].mac, mac, 6)) { f = i; break; }
  if (f < 0 && nSig < MAX_SIG) { f = nSig++; memcpy(sigs[f].mac, mac, 6); sigs[f].label[0] = 0; }
  if (f >= 0) {
    sigs[f].rssi = rssi; sigs[f].ble = ble; sigs[f].kind = kind;
    if (label && label[0]) snprintf(sigs[f].label, sizeof(sigs[f].label), "%s", label);
    sDirty = true;
  }
  portEXIT_CRITICAL(&smux);
}

// pull SSID (tag 0) out of tagged params starting at `off`
static void ssid_from(const uint8_t *p, int len, int off, char *dst, int dn) {
  dst[0] = 0;
  while (off + 2 <= len) {
    uint8_t id = p[off], l = p[off+1];
    if (off + 2 + l > len) break;
    if (id == 0) { char raw[33]; int n = l>32?32:l; memcpy(raw,p+off+2,n); raw[n]=0; ui_safe(dst,dn,raw); return; }
    off += 2 + l;
  }
}

static void sniff_pick(void *buf, wifi_promiscuous_pkt_type_t type) {
  wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
  wifi_hdr_t *h = (wifi_hdr_t *)pkt->payload;
  int len = pkt->rx_ctrl.sig_len; int8_t rssi = pkt->rx_ctrl.rssi;
  uint8_t ft = FTYPE(h->fctl), st = FSUBTYPE(h->fctl);

  if (ft == FT_DATA) { static uint32_t dc = 0; if ((dc++ & 7) != 0) return; }   // sample the flood

  char label[26];
  if (ft == FT_MGMT && (st == ST_BEACON || st == ST_PROBE_RESP)) {
    if (mcast(h->addr3)) return;
    label[0] = 0; if (len > 36) ssid_from(pkt->payload, len, 36, label, sizeof(label));
    if (!label[0]) snprintf(label, sizeof(label), "%s", oui_vendor(h->addr3));
    sig_upsert(h->addr3, label, rssi, false, 0);                 // AP
  } else if (ft == FT_MGMT && st == ST_PROBE_REQ) {
    if (mcast(h->addr2)) return;
    snprintf(label, sizeof(label), "%s", oui_vendor(h->addr2));
    sig_upsert(h->addr2, label, rssi, false, 1);                 // client
  } else if (ft == FT_DATA) {
    if (mcast(h->addr2)) return;
    snprintf(label, sizeof(label), "%s", oui_vendor(h->addr2));
    sig_upsert(h->addr2, label, rssi, false, 1);                 // client
  }
}

class PickCb : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice d) override {
    uint8_t *m = d.getAddress().getNative();
    char nm[26] = {0};
    if (d.haveName()) ui_safe(nm, sizeof(nm), d.getName().c_str());
    if (!nm[0]) snprintf(nm, sizeof(nm), "%s", oui_vendor(m));
    sig_upsert(m, nm, (int8_t)d.getRSSI(), true, 2);
  }
};
static PickCb pickCb;

// ---------------------------------------------------------------- TRACK: matchers
static void sniff_track(void *buf, wifi_promiscuous_pkt_type_t type) {
  wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
  wifi_hdr_t *h = (wifi_hdr_t *)pkt->payload;
  if (memcmp(h->addr1, g_target_mac, 6) && memcmp(h->addr2, g_target_mac, 6) &&
      memcmp(h->addr3, g_target_mac, 6)) return;
  portENTER_CRITICAL(&tmux);
  curRssi = pkt->rx_ctrl.rssi; haveRssi = true;
  seenChan = pkt->rx_ctrl.channel; seenAt = millis();
  portEXIT_CRITICAL(&tmux);
}
class TrackCb : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice d) override {
    uint8_t *m = d.getAddress().getNative();
    if (memcmp(m, g_target_mac, 6) != 0) return;
    portENTER_CRITICAL(&tmux);
    curRssi = d.getRSSI(); haveRssi = true; seenAt = millis();
    portEXIT_CRITICAL(&tmux);
  }
};
static TrackCb trackCb;

// forward
static void enter_pick(void);
static void enter_track(void);

// ---------------------------------------------------------------- PICK: UI
static void pick_row_cb(lv_event_t *e) {
  int r = (int)(intptr_t)lv_event_get_user_data(e);
  if (r < 0 || r >= nRow) return;
  buzz(15);
  tail_set_target(rowMac[r], rowLabel[r], rowBle[r]);            // lock it (just copies data)
  pendingMode = TRACK;                                           // defer: never clean inside an event cb
}
static void scroll_begin(lv_event_t *e){ LV_UNUSED(e); scrolling = true;  }
static void scroll_end(lv_event_t *e)  { LV_UNUSED(e); scrolling = false; }

static void build_pick(void) {
  statusLbl = lv_label_create(bodyRef);
  lv_obj_set_style_text_color(statusLbl, COL_TEXT_2, LV_PART_MAIN);
  lv_obj_set_style_text_font(statusLbl, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_label_set_text(statusLbl, "scanning for signals...");

  listBox = lv_obj_create(bodyRef);
  lv_obj_set_width(listBox, LV_PCT(100));
  lv_obj_set_height(listBox, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(listBox, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(listBox, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(listBox, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_row(listBox, 7, LV_PART_MAIN);
  lv_obj_set_flex_flow(listBox, LV_FLEX_FLOW_COLUMN);
  lv_obj_remove_flag(listBox, LV_OBJ_FLAG_SCROLLABLE);
}

static void rebuild_list(void) {
  if (!listBox) return;
  Sig snap[MAX_SIG]; int n;
  portENTER_CRITICAL(&smux);
  n = nSig; memcpy(snap, sigs, sizeof(Sig) * n); sDirty = false;
  portEXIT_CRITICAL(&smux);

  // sort strongest-first (insertion; n is small)
  for (int i = 1; i < n; i++) { Sig k = snap[i]; int j = i-1;
    while (j >= 0 && snap[j].rssi < k.rssi) { snap[j+1] = snap[j]; j--; } snap[j+1] = k; }
  if (n > SHOW_SIG) n = SHOW_SIG;

  lv_obj_clean(listBox);
  nRow = 0;
  if (statusLbl) lv_label_set_text_fmt(statusLbl, "%d signal%s  ·  tap to track  ·  ch %d",
                                       n, n==1?"":"s", radio_channel());
  const char *KIND[3] = { "AP", "client", "BLE" };
  for (int i = 0; i < n; i++) {
    char sub[64];
    snprintf(sub, sizeof(sub), "%s · %s · %ddBm", KIND[snap[i].kind], oui_vendor(snap[i].mac), snap[i].rssi);
    lv_obj_t *row = ui_row(listBox, snap[i].label[0] ? snap[i].label : "(hidden)", sub);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, pick_row_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    lv_obj_t *pip = lv_label_create(row);
    lv_label_set_text(pip, snap[i].ble ? LV_SYMBOL_BLUETOOTH : LV_SYMBOL_WIFI);
    lv_obj_align(pip, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_set_style_text_color(pip, rcol(snap[i].rssi), LV_PART_MAIN);
    lv_obj_set_style_text_font(pip, &lv_font_montserrat_16, LV_PART_MAIN);
    memcpy(rowMac[i], snap[i].mac, 6);
    snprintf(rowLabel[i], sizeof(rowLabel[i]), "%s", snap[i].label);
    rowBle[i] = snap[i].ble;
    nRow++;
  }
}

// ---------------------------------------------------------------- TRACK: UI
static void back_to_list_cb(lv_event_t *e) { LV_UNUSED(e); buzz(15); pendingMode = PICK; }  // deferred

static void build_track(void) {
  lblName = lv_label_create(bodyRef);
  lv_obj_set_width(lblName, LV_PCT(100));
  lv_obj_set_style_text_align(lblName, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_style_text_color(lblName, COL_TEXT, LV_PART_MAIN);
  lv_obj_set_style_text_font(lblName, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_label_set_long_mode(lblName, LV_LABEL_LONG_DOT);
  lv_label_set_text_fmt(lblName, "%s", g_target_name[0] ? g_target_name : "target");

  arc = lv_arc_create(bodyRef);
  lv_obj_set_size(arc, 210, 210);
  lv_obj_align(arc, LV_ALIGN_CENTER, 0, -6);
  lv_arc_set_rotation(arc, 270);
  lv_arc_set_bg_angles(arc, 0, 360);
  lv_arc_set_range(arc, 0, 100);
  lv_arc_set_value(arc, 0);
  lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_color(arc, COL_CARD_HI, LV_PART_MAIN);
  lv_obj_set_style_arc_color(arc, COL_GREEN, LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(arc, 16, LV_PART_MAIN);
  lv_obj_set_style_arc_width(arc, 16, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);
  lv_obj_set_style_pad_all(arc, 0, LV_PART_KNOB);

  lblRssi = lv_label_create(arc);
  lv_obj_center(lblRssi);
  lv_obj_set_style_text_color(lblRssi, COL_TEXT, LV_PART_MAIN);
  lv_obj_set_style_text_font(lblRssi, &lv_font_montserrat_40, LV_PART_MAIN);
  lv_label_set_text(lblRssi, "--");

  lblHint = lv_label_create(bodyRef);
  lv_obj_align_to(lblHint, arc, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);
  lv_obj_set_style_text_color(lblHint, COL_TEXT_2, LV_PART_MAIN);
  lv_obj_set_style_text_font(lblHint, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_label_set_text(lblHint, "searching...");

  // "back to signal list" button
  lv_obj_t *back = lv_obj_create(bodyRef);
  lv_obj_set_size(back, LV_PCT(70), 44);
  lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -6);
  lv_obj_set_style_bg_color(back, COL_CARD_HI, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(back, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(back, LV_OPA_70, LV_STATE_PRESSED);
  lv_obj_set_style_border_width(back, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(back, 12, LV_PART_MAIN);
  lv_obj_remove_flag(back, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(back, back_to_list_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *bl = lv_label_create(back);
  lv_label_set_text(bl, LV_SYMBOL_LIST "  pick another");
  lv_obj_center(bl);
  lv_obj_set_style_text_color(bl, COL_TEXT, LV_PART_MAIN);
  lv_obj_set_style_text_font(bl, &lv_font_montserrat_16, LV_PART_MAIN);
}

// ---------------------------------------------------------------- mode switches
static void enter_pick(void) {
  radio_stop(); ble_scan_stop();
  g_target_set = false;
  mode = PICK;
  nSig = 0; sDirty = false; nRow = 0; scrolling = false;
  lv_obj_clean(bodyRef);
  statusLbl = listBox = NULL; arc = lblRssi = lblName = lblHint = NULL;
  build_pick();
  radio_start(sniff_pick, true);
  ble_scan_start(&pickCb, true, 120, 100);
}

static void enter_track(void) {
  mode = TRACK;
  haveRssi = false; curRssi = -100; seenChan = 0; lastSeen = millis();
  lv_obj_clean(bodyRef);
  statusLbl = listBox = NULL; arc = lblRssi = lblName = lblHint = NULL;
  build_track();
  if (g_target_ble) { radio_stop();  ble_scan_start(&trackCb, true, 80, 70); }
  else              { ble_scan_stop(); radio_start(sniff_track, true); }
  USBSerial.printf("[tail] tracking %02X:%02X:%02X:%02X:%02X:%02X %s\n",
                   g_target_mac[0],g_target_mac[1],g_target_mac[2],
                   g_target_mac[3],g_target_mac[4],g_target_mac[5], g_target_ble?"(BLE)":"(WiFi)");
}

// ---------------------------------------------------------------- app entry points
void app_tail_open(lv_obj_t *body) {
  bodyRef = body;
  pendingMode = -1;
  // scroll guards registered once on the shell body (survive lv_obj_clean of children)
  lv_obj_add_event_cb(bodyRef, scroll_begin, LV_EVENT_SCROLL_BEGIN, NULL);
  lv_obj_add_event_cb(bodyRef, scroll_end,   LV_EVENT_SCROLL_END,   NULL);
  if (g_target_set) enter_track();      // external hook (SDR chain) pre-set a target
  else              enter_pick();
}

void app_tail_tick(void) {
  // Apply a deferred mode switch out here, never inside an event callback (cleaning
  // bodyRef while a row/button's own CLICKED is running would delete it mid-event).
  if (pendingMode >= 0) {
    Mode m = (Mode)pendingMode; pendingMode = -1;
    if (m == TRACK) enter_track(); else enter_pick();
    return;
  }

  uint32_t now = millis();

  if (mode == PICK) {
    radio_tick();                                       // hop while scanning
    static uint32_t last = 0;
    if (sDirty && now - last > 1500 && !scrolling && !g_ui_touching) { last = now; rebuild_list(); }
    return;
  }

  // TRACK
  if (!arc) return;
  if (!g_target_ble) {
    uint8_t sc; uint32_t sa;
    portENTER_CRITICAL(&tmux); sc = seenChan; sa = seenAt; portEXIT_CRITICAL(&tmux);
    if (sc && now - sa < 5000) { radio_set_hop(false); radio_set_channel(sc); }
    else                       { radio_set_hop(true); }
    radio_tick();
  }

  int rssi; bool have;
  portENTER_CRITICAL(&tmux); rssi = curRssi; have = haveRssi; haveRssi = false; portEXIT_CRITICAL(&tmux);

  if (have) {
    lastSeen = now;
    int pct = prox_pct(rssi);
    lv_arc_set_value(arc, pct);
    lv_label_set_text_fmt(lblRssi, "%d", rssi);
    lv_obj_set_style_arc_color(arc, rcol(rssi), LV_PART_INDICATOR);
    const char *word = rssi >= -50 ? "RIGHT HERE" : rssi >= -65 ? "very close"
                     : rssi >= -78 ? "nearby" : "faint";
    lv_label_set_text(lblHint, word);
    uint32_t interval = map(pct, 0, 100, 1200, 90);     // haptic geiger
    if (now - lastBuzz >= interval) { lastBuzz = now; buzz(pct > 80 ? 30 : 15); }
  } else if (now - lastSeen > 4000) {
    lv_label_set_text(lblHint, "lost signal");
    lv_arc_set_value(arc, 0);
    lv_label_set_text(lblRssi, "--");
  }
}

void app_tail_close(void) {
  radio_stop();
  ble_scan_stop();
  arc = lblRssi = lblName = lblHint = NULL;
  statusLbl = listBox = NULL;
  bodyRef = NULL;
  g_target_set = false;
}
