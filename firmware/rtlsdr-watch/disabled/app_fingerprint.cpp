// Fingerprint: name the device TYPE behind each radio around you. Combines OUI,
// the SSIDs a phone is probing for, and BLE names/appearance into a best guess
// (phone / tracker / earbuds / camera / wearable / TV...). Each unique device is
// appended once to /fingerprints.csv so the library grows over time. Pure RX.
#include "ui.h"
#include "oui.h"
#include "radio.h"
#include "ble.h"
#include "pin_config.h"
#include <SD_MMC.h>
#include <ctype.h>

#define MAXDEV 120
#define QN     16

struct Dev {
  uint8_t mac[6]; bool ble; char kind[26]; char vendor[14]; char detail[30]; uint16_t hits;
  int8_t  rssi;                // latest signal
  uint16_t lastwin;            // last correlation window this device was heard in
  int     parent;              // union-find: which profile this device belongs to
  int     anchor;              // current best co-located partner (device index)
  uint8_t ahits;               // how many windows in a row we've seen that same partner
  bool    pw;                  // profile already written to profiles.csv
};
static Dev devs[MAXDEV];
static volatile int nDev = 0;

// ---- correlation state ----
static uint16_t g_win    = 0;   // rolling ~12s window counter
static uint32_t g_profid = 0;   // profile id assigned on write

struct QItem { uint8_t mac[6]; bool ble; int8_t rssi; uint16_t app; char info[30]; };
static QItem q[QN];
static volatile int qh = 0, qt = 0;
static portMUX_TYPE fmux = portMUX_INITIALIZER_UNLOCKED;

static lv_obj_t *statusLbl = NULL, *listBox = NULL;
static File     csv;
static bool     sdOk = false;
static volatile bool scrolling = false;

static bool mcast(const uint8_t *m) { return (m[0] & 0x01); }

// case-insensitive substring (avoids the GNU strcasestr extension)
static bool ci_has(const char *hay, const char *needle) {
  if (!hay || !needle) return false;
  for (const char *p = hay; *p; p++) {
    const char *a = p, *b = needle;
    while (*a && *b && (tolower((unsigned char)*a) == tolower((unsigned char)*b))) { a++; b++; }
    if (!*b) return true;
  }
  return false;
}

static void enqueue(const uint8_t *mac, bool ble, int8_t rssi, uint16_t app, const char *info) {
  int nh = (qh + 1) % QN;
  if (nh == qt) return;                 // full, drop
  memcpy(q[qh].mac, mac, 6);
  q[qh].ble = ble; q[qh].rssi = rssi; q[qh].app = app;
  snprintf(q[qh].info, sizeof(q[qh].info), "%s", info ? info : "");
  qh = nh;
}

// ---- WiFi: probe requests are the fingerprint (what a device remembers) ----
static void sniff(void *buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT) return;
  wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
  wifi_hdr_t *h = (wifi_hdr_t *)pkt->payload;
  if (FTYPE(h->fctl) != FT_MGMT || FSUBTYPE(h->fctl) != ST_PROBE_REQ) return;
  if (mcast(h->addr2)) return;
  char ssid[30] = {0};
  const uint8_t *p = pkt->payload + 24;
  int len = pkt->rx_ctrl.sig_len - 24 - 4;
  if (len >= 2 && p[0] == 0) {
    int sl = p[1]; if (sl > 28) sl = 28;
    if (sl > 0 && 2 + sl <= len) {
      int j = 0;
      for (int i = 0; i < sl; i++) { char c = p[2+i]; if (c >= 32 && c <= 126) ssid[j++] = c; }
      ssid[j] = 0;
    }
  }
  portENTER_CRITICAL(&fmux); enqueue(h->addr2, false, pkt->rx_ctrl.rssi, 0, ssid); portEXIT_CRITICAL(&fmux);
}

// ---- BLE: name + appearance refine the guess ----
class FpCb : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice d) override {
    uint8_t *m = d.getAddress().getNative();
    char nm[30] = {0};
    if (d.haveName()) { char raw[40]; snprintf(raw, sizeof(raw), "%s", d.getName().c_str());
                        int j = 0; for (int i = 0; raw[i] && j < 28; i++) if (raw[i] >= 32 && raw[i] <= 126) nm[j++] = raw[i]; nm[j] = 0; }
    uint16_t app = d.haveAppearance() ? d.getAppearance() : 0;
    portENTER_CRITICAL(&fmux); enqueue(m, true, d.getRSSI(), app, nm); portEXIT_CRITICAL(&fmux);
  }
};
static FpCb cb;

static void classify(Dev *d, const QItem *it) {
  const char *v = oui_vendor(d->mac);
  snprintf(d->vendor, sizeof(d->vendor), "%s", v);
  const char *kind = device_kind(v, it->ble);
  if (it->ble) {
    const char *nm = it->info;
    if      (ci_has(nm, "airpod"))  kind = "AirPods";
    else if (ci_has(nm, "buds"))    kind = "earbuds";
    else if (ci_has(nm, "tile"))    kind = "Tile tracker";
    else if (ci_has(nm, "ring"))    kind = "Ring device";
    else if (ci_has(nm, "govee"))   kind = "Govee sensor";
    else if (ci_has(nm, "watch"))   kind = "smartwatch";
    else if (ci_has(nm, "band"))    kind = "fitness band";
    else if (ci_has(nm, "tv") || ci_has(nm, "roku") || ci_has(nm, "bravia")) kind = "TV";
    else if (ci_has(nm, "cam"))     kind = "camera";
    if (it->app) {
      uint16_t a = it->app;
      if      (a >= 0x0040 && a <= 0x0048) kind = "phone";
      else if (a == 0x00C0 || a == 0x00C1 || a == 0x00C2) kind = "wearable";
      else if (a >= 0x0340 && a <= 0x0348) kind = "heart-rate";
    }
    snprintf(d->detail, sizeof(d->detail), "%s", (nm[0]) ? nm : "(no name)");
  } else {
    if (it->info[0]) snprintf(d->detail, sizeof(d->detail), "wants \"%s\"", it->info);
    else             snprintf(d->detail, sizeof(d->detail), "probing");
  }
  snprintf(d->kind, sizeof(d->kind), "%s", kind);
}

static void csv_append(const Dev *d) {
  if (!csv) return;
  csv.printf("%04u-%02u-%02u %02u:%02u:%02u,%04u-%02u-%02u %02u:%02u:%02u,"
             "%02X:%02X:%02X:%02X:%02X:%02X,%s,%s,%s,\"%s\",%u\n",
             SYS.year,SYS.month,SYS.day,SYS.hh,SYS.mm,SYS.ss,
             SYS.year,SYS.month,SYS.day,SYS.hh,SYS.mm,SYS.ss,
             d->mac[0],d->mac[1],d->mac[2],d->mac[3],d->mac[4],d->mac[5],
             d->ble ? "BLE" : "WiFi", d->kind, d->vendor, d->detail, d->hits);
}

// ---- correlation: group radios that travel together into one profile ----
static int uf_find(int i) {
  while (devs[i].parent != i) { devs[i].parent = devs[devs[i].parent].parent; i = devs[i].parent; }
  return i;
}
static void uf_union(int a, int b) { a = uf_find(a); b = uf_find(b); if (a != b) devs[b].parent = a; }

// Append one correlated profile (union-find root `r`, >=2 radios) to profiles.csv.
static void append_profile(int r) {
  File f = SD_MMC.open("/profiles.csv", FILE_APPEND);
  if (!f) return;
  char macs[170] = {0}, kinds[120] = {0}, names[160] = {0}, ssids[160] = {0};
  int cnt = 0; const char *label = "device";
  for (int i = 0; i < nDev; i++) {
    if (uf_find(i) != r) continue;
    cnt++;
    char one[26];
    snprintf(one, sizeof(one), "%s%02X:%02X:%02X:%02X:%02X:%02X", macs[0] ? ";" : "",
             devs[i].mac[0],devs[i].mac[1],devs[i].mac[2],devs[i].mac[3],devs[i].mac[4],devs[i].mac[5]);
    strncat(macs, one, sizeof(macs) - strlen(macs) - 1);
    if (!ci_has(kinds, devs[i].kind)) {
      char k[28]; snprintf(k, sizeof(k), "%s%s", kinds[0] ? ";" : "", devs[i].kind);
      strncat(kinds, k, sizeof(kinds) - strlen(kinds) - 1);
    }
    if (devs[i].ble && devs[i].detail[0] && devs[i].detail[0] != '(') {
      char n[32]; snprintf(n, sizeof(n), "%s%s", names[0] ? ";" : "", devs[i].detail);
      strncat(names, n, sizeof(names) - strlen(names) - 1);
    }
    if (!devs[i].ble && !strncmp(devs[i].detail, "wants", 5)) {
      char s[32]; snprintf(s, sizeof(s), "%s%s", ssids[0] ? ";" : "", devs[i].detail + 6);
      strncat(ssids, s, sizeof(ssids) - strlen(ssids) - 1);
    }
    label = devs[i].kind;
  }
  f.printf("%lu,%04u-%02u-%02u %02u:%02u:%02u,%04u-%02u-%02u %02u:%02u:%02u,%d,%s,\"%s\",\"%s\",\"%s\",\"%s\"\n",
           (unsigned long)(++g_profid),
           SYS.year,SYS.month,SYS.day,SYS.hh,SYS.mm,SYS.ss,
           SYS.year,SYS.month,SYS.day,SYS.hh,SYS.mm,SYS.ss,
           cnt, label, kinds, macs, names, ssids);
  f.close();
}

// One correlation window (~12s): radios heard together at similar RSSI across two
// windows in a row get linked. Repetition is the filter -- a one-off in a crowd
// doesn't link, an iPhone + its AirPods seen together twice does.
static void corr_window(void) {
  int act[MAXDEV], na = 0;
  for (int i = 0; i < nDev; i++) if (devs[i].lastwin == g_win) act[na++] = i;
  for (int a = 0; a < na; a++) {
    int i = act[a], bestj = -1, bestd = 999;
    for (int b = 0; b < na; b++) {
      if (a == b) continue;
      int j = act[b];
      int dd = abs((int)devs[i].rssi - (int)devs[j].rssi);
      if (dd <= 6 && dd < bestd) { bestd = dd; bestj = j; }
    }
    if (bestj >= 0) {
      if (devs[i].anchor == bestj) { if (devs[i].ahits < 255) devs[i].ahits++; }
      else { devs[i].anchor = bestj; devs[i].ahits = 1; }
      if (devs[i].ahits >= 2) uf_union(i, bestj);
    }
  }
  g_win++;

  if (!sdOk) return;
  for (int r = 0; r < nDev; r++) {
    if (uf_find(r) != r || devs[r].pw) continue;
    int cnt = 0; for (int i = 0; i < nDev; i++) if (uf_find(i) == r) cnt++;
    if (cnt >= 2) { devs[r].pw = true; append_profile(r); }
  }
}

static lv_color_t rcol(int r){ return r>=-55?COL_GREEN : r>=-72?COL_YELLOW : COL_ORANGE; }
static void scroll_begin(lv_event_t *e){ LV_UNUSED(e); scrolling = true;  }
static void scroll_end(lv_event_t *e)  { LV_UNUSED(e); scrolling = false; }

void app_fingerprint_open(lv_obj_t *body) {
  nDev = 0; qh = qt = 0; scrolling = false; g_win = 0; g_profid = 0;
  lv_obj_add_event_cb(body, scroll_begin, LV_EVENT_SCROLL_BEGIN, NULL);
  lv_obj_add_event_cb(body, scroll_end,   LV_EVENT_SCROLL_END,   NULL);

  statusLbl = lv_label_create(body);
  lv_obj_set_style_text_color(statusLbl, COL_TEXT_2, LV_PART_MAIN);
  lv_obj_set_style_text_font(statusLbl, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_label_set_text(statusLbl, "identifying devices...");

  SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0);
  sdOk = SD_MMC.begin("/sdcard", true, false);
  if (sdOk) {
    bool fresh = !SD_MMC.exists("/fingerprints.csv");
    csv = SD_MMC.open("/fingerprints.csv", FILE_APPEND);
    if (csv) { if (fresh) csv.print("first_seen,last_seen,mac,radio,kind,vendor,detail,hits\n"); csv.flush(); }
    else sdOk = false;
    if (sdOk && !SD_MMC.exists("/profiles.csv")) {
      File pf = SD_MMC.open("/profiles.csv", FILE_WRITE);
      if (pf) { pf.print("profile_id,first_seen,last_seen,seen_count,label,device_types,macs,names,probed_ssids\n"); pf.close(); }
    }
  }

  listBox = lv_obj_create(body);
  lv_obj_set_width(listBox, LV_PCT(100));
  lv_obj_set_height(listBox, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(listBox, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(listBox, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(listBox, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_row(listBox, 7, LV_PART_MAIN);
  lv_obj_set_flex_flow(listBox, LV_FLEX_FLOW_COLUMN);
  lv_obj_remove_flag(listBox, LV_OBJ_FLAG_SCROLLABLE);

  radio_start(sniff, true);
  ble_scan_start(&cb, true, 120, 100);
}

static void add_row(const Dev *d) {
  char title[40], sub[80];
  snprintf(title, sizeof(title), "%s", d->kind);
  snprintf(sub, sizeof(sub), "%s · %02X:%02X:%02X · %s",
           d->vendor, d->mac[0], d->mac[1], d->mac[2], d->detail);
  lv_obj_t *r = ui_row(listBox, title, sub);
  lv_obj_t *p = lv_label_create(r);
  lv_label_set_text(p, d->ble ? LV_SYMBOL_BLUETOOTH : LV_SYMBOL_WIFI);
  lv_obj_align(p, LV_ALIGN_RIGHT_MID, -4, 0);
  lv_obj_set_style_text_color(p, d->ble ? COL_INDIGO : COL_BLUE, LV_PART_MAIN);
  lv_obj_set_style_text_font(p, &lv_font_montserrat_16, LV_PART_MAIN);
}

void app_fingerprint_tick(void) {
  radio_tick();

  // drain the queue in loop context (classify + dedup + persist + show)
  while (qt != qh) {
    QItem it;
    portENTER_CRITICAL(&fmux); it = q[qt]; qt = (qt + 1) % QN; portEXIT_CRITICAL(&fmux);

    int found = -1;
    for (int i = 0; i < nDev; i++) if (!memcmp(devs[i].mac, it.mac, 6)) { found = i; break; }
    if (found >= 0) {
      if (devs[found].hits < 65535) devs[found].hits++;
      devs[found].rssi = it.rssi; devs[found].lastwin = g_win;   // heard again this window
      continue;
    }
    if (nDev >= MAXDEV) continue;

    Dev *d = &devs[nDev];
    memcpy(d->mac, it.mac, 6); d->ble = it.ble; d->hits = 1;
    d->rssi = it.rssi; d->lastwin = g_win;
    d->parent = nDev; d->anchor = -1; d->ahits = 0; d->pw = false;
    classify(d, &it);
    nDev++;
    if (!scrolling && !g_ui_touching) add_row(d);
    if (sdOk) csv_append(d);
  }
  if (sdOk && csv) { static uint32_t lf = 0; uint32_t n = millis(); if (n - lf > 2000) { lf = n; csv.flush(); } }

  static uint32_t lastW = 0;                       // run a correlation window ~every 12s
  if (millis() - lastW > 12000) { lastW = millis(); corr_window(); }

  static uint32_t last = 0;
  uint32_t now = millis();
  if (now - last > 700) { last = now;
    if (statusLbl) lv_label_set_text_fmt(statusLbl, "%d device%s identified%s · ch %d",
                                         nDev, nDev == 1 ? "" : "s",
                                         sdOk ? " · logging" : " · no SD", radio_channel());
  }
}

void app_fingerprint_close(void) {
  radio_stop();
  ble_scan_stop();
  if (csv) { csv.flush(); csv.close(); }
  corr_window();                 // final pass -> flush any last-formed profiles
  if (sdOk) SD_MMC.end();
  sdOk = false;
  statusLbl = listBox = NULL;
  nDev = 0;
}
