#include "radio.h"
#include <WiFi.h>
#include "esp_wifi.h"
#include "esp_private/wifi.h"   // esp_wifi_internal_tx bypasses the mgmt-frame sanity check

// NOTE: on esp32 core 3.3.11 the SDK's ieee80211_raw_frame_sanity_check is a
// STRONG symbol, so it can't be overridden from a normal Arduino build. That
// means raw deauth/disassoc frames are filtered by esp_wifi_80211_tx and won't
// leave the radio. Beacon frames (mgmt subtype 8) are NOT filtered and transmit
// fine. radio_tx() returns the esp_err so callers can report what actually went
// out rather than pretend. We route raw TX through esp_wifi_internal_tx, which
// does not run that sanity check, so deauth does transmit.

static bool     started = false;
static bool     hopping = false;
static uint8_t  chan    = 1;
static uint32_t lastHop = 0;

void radio_set_channel(uint8_t ch) {
  if (ch < 1) ch = 1;
  if (ch > 13) ch = 13;
  chan = ch;
  esp_wifi_set_channel(chan, WIFI_SECOND_CHAN_NONE);
}

uint8_t radio_channel(void) { return chan; }
void    radio_set_hop(bool h) { hopping = h; }

void radio_start(wifi_promiscuous_cb_t cb, bool hop) {
  hopping = hop;
  WiFi.mode(WIFI_STA);                   // idempotent: brings WiFi up, and recovers it
                                         // if another app (Evidence) had set WIFI_OFF
  started = true;
  wifi_promiscuous_filter_t f = { .filter_mask = WIFI_PROMIS_FILTER_MASK_ALL };
  esp_wifi_set_promiscuous_filter(&f);
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(cb);    // just swap the callback between tools
  radio_set_channel(chan);
  lastHop = millis();
}

// Does NOT stop the WiFi stack. Fully stopping and restarting WiFi between apps
// (esp_wifi_stop + WiFi.mode(OFF), then back on when the next tool opens) HANGS
// under WiFi+BLE coexistence in a dense RF environment -- that was the freeze when
// you tap a network in Profiler (it hands off to Tail, which restarted the radio).
// We only drop out of promiscuous mode and clear the callback; WiFi stays up.
void radio_stop(void) {
  if (!started) return;
  esp_wifi_set_promiscuous_rx_cb(NULL);
  esp_wifi_set_promiscuous(false);
  hopping = false;
}

void radio_tick(void) {
  if (!started || !hopping) return;
  uint32_t now = millis();
  if (now - lastHop < 250) return;       // ~4 channels/sec, slow enough to catch beacons
  lastHop = now;
  chan++;
  if (chan > 13) chan = 1;
  esp_wifi_set_channel(chan, WIFI_SECOND_CHAN_NONE);
}

// Protection / Cloak modes forbid the radio from transmitting anything.
extern bool g_mode_protection;
extern bool g_mode_cloak;

int radio_tx(const uint8_t *frame, int len) {
  if (!started) return -1;
  if (g_mode_protection || g_mode_cloak) return -3;   // RF-silent while hardened/cloaked
  // internal_tx does NOT run ieee80211_raw_frame_sanity_check, so deauth/disassoc
  // actually leave the radio on core 3.3.11. Fall back to the public API if the
  // internal symbol ever changes.
  int r = esp_wifi_internal_tx(WIFI_IF_STA, (void *)frame, len);
  if (r != 0) r = esp_wifi_80211_tx(WIFI_IF_STA, frame, len, false);
  return r;
}
