// The watch's ESP-NOW link to the RTL-SDR system. See sdr_link.h for the model.
#include "sdr_link.h"
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "radio.h"     // radio_stop(): drop any promiscuous cb/hop a prior app left on

static const uint8_t ENOW_BCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
static bool enowUp = false;

// ESP-NOW RX byte ring: producer = Wi-Fi task callback, consumer = sdr_link_poll().
#define SRX_SZ 2048
static volatile uint16_t srxHead = 0, srxTail = 0;
static uint8_t  srxBuf[SRX_SZ];

static LinkParser rxParser;          // fed only from sdr_link_poll() (main loop)
static SdrState   g_state;           // written only from sdr_link_poll() (main loop)
static uint8_t    seq = 0;
static uint8_t    txbuf[LINK_ENOW_MAX_PAY];

// Wi-Fi task context: validate magic, copy the lp bytes into the ring. Nothing else.
static void onEnowRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  (void)info;
  if (len <= (int)LINK_ENOW_MAGIC_LEN) return;
  if (data[0]!=LINK_ENOW_MAGIC0 || data[1]!=LINK_ENOW_MAGIC1 ||
      data[2]!=LINK_ENOW_MAGIC2 || data[3]!=LINK_ENOW_MAGIC3) return;
  for (int i=(int)LINK_ENOW_MAGIC_LEN; i<len; i++) {
    uint16_t nh = (uint16_t)((srxHead + 1) & (SRX_SZ - 1));
    if (nh == srxTail) break;         // ring full: drop rest (poll will catch up)
    srxBuf[srxHead] = data[i];
    srxHead = nh;
  }
}

void sdr_link_begin(void) {
  memset(&g_state, 0, sizeof(g_state));
  rxParser.reset();
  srxHead = srxTail = 0;
  radio_stop();                              // clear promiscuous cb + hop if an app left them
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();                         // never associate to an AP (would move the channel)
  delay(100);
  esp_wifi_set_ps(WIFI_PS_NONE);             // no power-save: never sleep-miss a control packet
  esp_wifi_set_promiscuous(false);
  esp_wifi_set_channel(LINK_ENOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() != ESP_OK) { enowUp = false; return; }
  esp_now_register_recv_cb(onEnowRecv);
  esp_now_peer_info_t peer; memset(&peer, 0, sizeof(peer));
  memcpy(peer.peer_addr, ENOW_BCAST, 6);
  peer.channel = 0;                          // 0 = use the STA's current channel
  peer.ifidx   = WIFI_IF_STA;
  peer.encrypt = false;
  esp_now_add_peer(&peer);
  enowUp = true;
}

void sdr_link_end(void) {
  if (!enowUp) return;
  esp_now_unregister_recv_cb();
  esp_now_del_peer(ENOW_BCAST);
  esp_now_deinit();
  enowUp = false;
  // Leave Wi-Fi UP so other apps reuse it; do NOT esp_wifi_stop (hangs under coex).
}

static void applyFrame(uint8_t msg, const uint8_t* p, uint16_t len) {
  g_state.lastRxMs = millis();
  g_state.linkUp   = true;
  g_state.frames++;
  switch (msg) {
    case MSG_HELLO_TEENSY:
      if (len >= 3) { g_state.fwMajor = p[1]; g_state.fwMinor = p[2]; }
      break;
    case MSG_TELEMETRY:
      if (len >= 67) {
        g_state.tunedHz     = lp_get32(&p[0]);   g_state.sampleRateHz = lp_get32(&p[4]);
        g_state.mode        = p[8];              g_state.gainAuto     = p[9];
        g_state.gainTenthDb = lp_geti16(&p[10]); g_state.ppm          = lp_geti16(&p[12]);
        g_state.biasTee     = p[14];             g_state.directSamp   = p[15];
        g_state.demod       = p[16];             g_state.tunerType    = p[17];
        g_state.sdrPresent  = p[18];             g_state.sdrStreaming = p[19];
        g_state.rssiTenthDbfs = lp_geti16(&p[20]); g_state.floorTenthDbfs = lp_geti16(&p[22]);
        g_state.busMv       = lp_get16(&p[24]);  g_state.curMa        = lp_geti16(&p[26]);
        g_state.powCw       = lp_get16(&p[28]);  g_state.dieTenthC    = lp_geti16(&p[30]);
        g_state.fanDuty     = p[32];             g_state.sdOk         = p[33];
        g_state.gpsValid    = p[52];             g_state.gpsSats      = p[53];
        g_state.latE7       = lp_geti32(&p[54]); g_state.lonE7        = lp_geti32(&p[58]);
        g_state.altM        = lp_geti16(&p[62]); g_state.hh = p[64]; g_state.mm = p[65]; g_state.ss = p[66];
        if (len >= TLM_LEN) {
          g_state.txActive = p[67]; g_state.txMode = p[68];
          g_state.txFreqHz = lp_get32(&p[69]);
          g_state.fanRpm   = lp_get16(&p[73]);
        }
        g_state.tlmValid    = true;
      }
      break;
    case MSG_SPECTRUM:
      if (len >= SPEC_HDR) {
        g_state.specCentre = lp_get32(&p[0]); g_state.specSpan = lp_get32(&p[4]);
        uint16_t nb = lp_get16(&p[8]); if (nb > LINK_WATCH_SPEC_BINS) nb = LINK_WATCH_SPEC_BINS;
        g_state.specRefT = lp_geti16(&p[10]); g_state.specFloorT = lp_geti16(&p[12]);
        uint16_t avail = (uint16_t)(len - SPEC_HDR); if (avail < nb) nb = avail;
        memcpy(g_state.specMag, &p[SPEC_HDR], nb);
        g_state.specBins = nb;
        g_state.specNew  = true;
      }
      break;
    case MSG_DETECT:
      if (len >= DET_LEN) {
        g_state.detFreqHz = lp_get32(&p[0]); g_state.detPowT = lp_geti16(&p[4]);
        g_state.detBwKhz  = lp_get16(&p[6]); g_state.detKind = p[12];
        g_state.detNew    = true;
      }
      break;
    default: break;
  }
}

void sdr_link_poll(void) {
  uint8_t oseq, oflags, ochan, omsg; const uint8_t* opay; uint16_t oplen;
  int budget = SRX_SZ;
  while (srxTail != srxHead && budget-- > 0) {
    uint8_t c = srxBuf[srxTail];
    srxTail = (uint16_t)((srxTail + 1) & (SRX_SZ - 1));
    if (rxParser.feed(c, oseq, oflags, ochan, omsg, opay, oplen)) applyFrame(omsg, opay, oplen);
  }
  if (g_state.linkUp && (millis() - g_state.lastRxMs) > LINK_TIMEOUT_MS) g_state.linkUp = false;
}

const SdrState* sdr_link_state(void) { return &g_state; }

// ------------------------------------------------------------------ commands out
static void enowSend(uint8_t msg, const uint8_t* pay, uint16_t plen) {
  if (!enowUp) return;
  txbuf[0]=LINK_ENOW_MAGIC0; txbuf[1]=LINK_ENOW_MAGIC1;
  txbuf[2]=LINK_ENOW_MAGIC2; txbuf[3]=LINK_ENOW_MAGIC3;
  uint32_t n = lp_encode(&txbuf[LINK_ENOW_MAGIC_LEN], seq++, 0, LINK_CHAN_WATCH, msg, pay, plen);
  esp_now_send(ENOW_BCAST, txbuf, (size_t)(LINK_ENOW_MAGIC_LEN + n));
}
void sdr_send_cmd(uint8_t cmd, const uint8_t* a, uint16_t alen) {
  uint8_t p[64]; if (alen > 63) alen = 63; p[0] = cmd; if (alen && a) memcpy(&p[1], a, alen);
  enowSend(MSG_CMD, p, (uint16_t)(1 + alen));
}
void sdr_send_req(uint8_t what) { uint8_t p = what; enowSend(MSG_REQ, &p, 1); }
void sdr_cmd_u32(uint8_t cmd, uint32_t v) { uint8_t a[4]; lp_put32(a, v);  sdr_send_cmd(cmd, a, 4); }
void sdr_cmd_i32(uint8_t cmd, int32_t v)  { uint8_t a[4]; lp_puti32(a, v); sdr_send_cmd(cmd, a, 4); }
void sdr_cmd_u8(uint8_t cmd, uint8_t v)   { sdr_send_cmd(cmd, &v, 1); }
void sdr_cmd_set_gain(bool autoG, int16_t tdb) {
  uint8_t a[3]; a[0] = autoG ? 1 : 0; lp_puti16(&a[1], tdb); sdr_send_cmd(CMD_SET_GAIN, a, 3);
}
void sdr_cmd_band(uint32_t hz, uint32_t sr, uint8_t demod) {
  sdr_cmd_u32(CMD_SET_SR, sr);
  sdr_cmd_u32(CMD_SET_FREQ, hz);
  sdr_cmd_u8(CMD_SET_DEMOD, demod);
}
void sdr_cmd_tx_set(uint8_t mode, uint32_t freqHz) {
  uint8_t a[5]; a[0]=mode; lp_put32(&a[1], freqHz); sdr_send_cmd(CMD_TX_SET, a, 5);
}
void sdr_cmd_tx_key(bool on) { sdr_cmd_u8(CMD_TX_KEY, on ? 1 : 0); }
