// ============================================================================
//  sdr_link.h -- the S3 AMOLED watch's wireless link to the RTL-SDR system.
//
//  The watch is the CONTROLLER. It speaks the shared link protocol
//  (link_proto.h) over ESP-NOW to the E32R40T, which BRIDGES it onto the wired
//  UART to the Teensy engine. The watch sends control commands down and mirrors
//  the engine's telemetry / spectrum / detections coming back up.
//
//  ESP-NOW packet = [4-byte MAGIC][one lp-frame]. Both ends pin
//  LINK_ENOW_CHANNEL, no AP, no channel hopping. Broadcast peer (no MAC pairing).
//
//  Threading: the ESP-NOW receive callback runs on the Wi-Fi task and only
//  copies bytes into a lock-free ring. All parsing and all state updates happen
//  in sdr_link_poll(), which the SDR app calls from its tick() (main loop), so
//  the LVGL side never races the radio task -- the same discipline ble.cpp uses.
// ============================================================================
#pragma once
#include <Arduino.h>
#include "link_proto.h"

struct SdrState {
  bool     linkUp;                 // a valid frame arrived within LINK_TIMEOUT_MS
  uint32_t lastRxMs;
  uint32_t frames;                 // good frames seen (link-quality readout)

  // ---- telemetry mirror (see TLM_LEN layout in link_proto.h) ----
  bool     tlmValid;
  uint32_t tunedHz, sampleRateHz;
  uint8_t  mode, gainAuto;  int16_t gainTenthDb, ppm;
  uint8_t  biasTee, directSamp, demod, tunerType, sdrPresent, sdrStreaming;
  int16_t  rssiTenthDbfs, floorTenthDbfs;
  uint16_t busMv;  int16_t curMa;  uint16_t powCw;  int16_t dieTenthC;
  uint8_t  fanDuty, sdOk;
  uint8_t  gpsValid, gpsSats;  int32_t latE7, lonE7;  int16_t altM;  uint8_t hh, mm, ss;
  uint8_t  fwMajor, fwMinor;

  // ---- GPIO RF transmitter state ----
  uint8_t  txActive, txMode;  uint32_t txFreqHz;

  // ---- fan tach ----
  uint16_t fanRpm;

  // ---- spectrum (already decimated by the bridge to <= LINK_WATCH_SPEC_BINS) ----
  bool     specNew;
  uint32_t specCentre, specSpan;  uint16_t specBins;
  int16_t  specRefT, specFloorT;
  uint8_t  specMag[LINK_WATCH_SPEC_BINS];

  // ---- last detection ----
  bool     detNew;  uint32_t detFreqHz;  int16_t detPowT;  uint16_t detBwKhz;  uint8_t detKind;
};

void  sdr_link_begin(void);              // WiFi STA + ESP-NOW up, channel pinned
void  sdr_link_end(void);                // ESP-NOW down (Wi-Fi left up for other apps)
void  sdr_link_poll(void);               // drain ring + parse + update state; call from tick()
const SdrState* sdr_link_state(void);    // read-only view (main loop only)

// ---- control commands to the SDR (relayed by the bridge to the Teensy) ----
void  sdr_send_cmd(uint8_t cmd, const uint8_t* a, uint16_t alen);
void  sdr_send_req(uint8_t what);
void  sdr_cmd_u32(uint8_t cmd, uint32_t v);
void  sdr_cmd_i32(uint8_t cmd, int32_t v);
void  sdr_cmd_u8(uint8_t cmd, uint8_t v);
void  sdr_cmd_set_gain(bool autoG, int16_t tenthDb);
void  sdr_cmd_band(uint32_t hz, uint32_t sr, uint8_t demod);
void  sdr_cmd_tx_set(uint8_t mode, uint32_t freqHz);
void  sdr_cmd_tx_key(bool on);
