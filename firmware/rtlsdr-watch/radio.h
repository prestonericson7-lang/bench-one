// Shared 802.11 radio core: promiscuous capture, channel hop, raw TX.
// One app uses this at a time (the launcher tears an app down before opening
// the next), so there is a single global sniffer callback slot.
#pragma once
#include <Arduino.h>
#include "esp_wifi.h"

// Start WiFi in a NULL/promiscuous state and install `cb`. If hop is true,
// radio_tick() rotates channels 1..13. Safe to call when already started.
void    radio_start(wifi_promiscuous_cb_t cb, bool hop);
void    radio_stop(void);
void    radio_tick(void);              // call from the app's tick to advance the hop
void    radio_set_channel(uint8_t ch); // 1..13, pins the channel (also stops hop drift)
uint8_t radio_channel(void);
void    radio_set_hop(bool hop);

// Raw 802.11 injection. Returns esp_err_t. Deauth/disassoc frames are allowed
// (see the sanity-check override in radio.cpp).
int     radio_tx(const uint8_t *frame, int len);

// Common 802.11 header offsets for parsers.
struct __attribute__((packed)) wifi_hdr_t {
  uint16_t fctl;
  uint16_t duration;
  uint8_t  addr1[6];   // receiver / dest
  uint8_t  addr2[6];   // transmitter / source
  uint8_t  addr3[6];   // bssid
  uint16_t seq;
};

#define FTYPE(fctl)   (((fctl) >> 2) & 0x3)
#define FSUBTYPE(fctl)(((fctl) >> 4) & 0xF)
#define FT_MGMT   0
#define FT_CTRL   1
#define FT_DATA   2
#define ST_ASSOC_REQ   0
#define ST_PROBE_REQ   4
#define ST_BEACON      8
#define ST_PROBE_RESP  5
#define ST_DEAUTH     12
#define ST_DISASSOC   10
#define ST_QOS_DATA    8
