// ============================================================================
//  link_proto.h -- Teensy 4.1 <-> E32R40T display link protocol
//
//  SHARED, HEADER-ONLY. This exact file is compiled on BOTH ends:
//    - firmware/rtlsdr-pentest   (Teensy 4.1, the SDR engine + button input)
//    - firmware/rtlsdr-display   (E32R40T ESP32-WROOM-32E, the UI computer)
//  Keep the two copies byte-identical. If you change one, copy it to the other.
//
//  ROLE SPLIT (set by the operator):
//    * Teensy + RTL-SDR do the work: USB IQ streaming, FFT/DSP, signal
//      detection, SD capture, INA219 power, die-temp/fan, and PHYSICAL BUTTONS.
//      The Teensy pushes semantic DATA up (spectrum, detections, telemetry) and
//      forwards button events; it EXECUTES control commands.
//    * E32R40T runs the UI: it owns the menu/screen state machine and every
//      pixel of layout + the waterfall renderer, so the Teensy is never blocked
//      composing screens while it should be servicing the SDR. It consumes the
//      data + button events and sends SDR CONTROL COMMANDS back down.
//    * The panel's resistive touch is DEAD on this unit -> there is no touch
//      path. All input is Teensy-side buttons, forwarded as INPUT events.
//
//  WIRE FORMAT (little-endian payloads):
//    0xAA  LEN_H  LEN_L  SEQ  FLAGS  CHAN  MSG  payload[LEN]  CRC_H  CRC_L
//      - LEN   = payload byte count, 0..LINK_MAX_PAYLOAD (big-endian in header)
//      - SEQ   = rolling frame counter (wraps 0..255), for loss diagnostics
//      - FLAGS = bit0 FULL_REPAINT hint; other bits reserved (0)
//      - CHAN  = LINK_CHAN_* (one channel today: DISPLAY = 0x05)
//      - MSG   = LinkMsg
//      - CRC   = CRC-16/MCRF4XX over [SEQ,FLAGS,CHAN,MSG,payload], big-endian.
//               reflected poly 0x8408, init 0xFFFF, no final xor.
//               Self-test: CRC("123456789") == 0x6F91 (verified).
//
//  Numeric fields inside payloads are LITTLE-endian (put16/put32 below); only
//  the three framing words (LEN, CRC) are big-endian, matching the research note
//  and so a byte sniffer reads the length and CRC left-to-right.
// ============================================================================
#pragma once
#include <stdint.h>
#include <string.h>

// ------------------------------------------------------------------ constants
#define LINK_BAUD          921600UL
#define LINK_START         0xAAu
#define LINK_MAX_PAYLOAD   1024u          // one spectrum frame of 1024 bins fits
#define LINK_MAX_FRAME     (7u + LINK_MAX_PAYLOAD + 2u)

#define LINK_CHAN_DISPLAY  0x05u          // research note: 0x05, not the reserved 0x03

#define LINK_FLAG_FULL     0x01u          // repaint hint for screen-changing frames

// Link timing: the display marks the link DOWN if no frame arrives for this long.
#define LINK_TIMEOUT_MS    1500u
#define LINK_HELLO_MS      500u           // heartbeat cadence when otherwise idle

// ------------------------------------------------------------------ messages
// Teensy -> Display are >= 0x10.  Display -> Teensy are < 0x10.  PING/PONG both.
enum LinkMsg : uint8_t {
  // ---- transport (either direction) ----
  MSG_PING        = 0x00,   // payload: u32 token -> other side must PONG it back
  MSG_PONG        = 0x01,   // payload: u32 token echoed

  // ---- Display -> Teensy ----
  MSG_HELLO_DISP  = 0x02,   // display announces: u8 proto, u8 resetReason, u8 panelOk, u8 touchOk
  MSG_CMD         = 0x03,   // a control command from the UI (see LinkCmd payload below)
  MSG_REQ         = 0x04,   // a data request from the UI (see LinkReq below)

  // ---- Teensy -> Display ----
  MSG_HELLO_TEENSY= 0x10,   // teensy announces: u8 proto, u8 fwMajor, u8 fwMinor, u8 sdrPresent
  MSG_TELEMETRY   = 0x11,   // full system telemetry snapshot (LinkTelemetry, packed)
  MSG_SPECTRUM    = 0x12,   // spectrum frame: header + N bytes of 0..255 magnitude
  MSG_DETECT      = 0x13,   // one detected signal (LinkDetect, packed) to add to the list
  MSG_ACK         = 0x14,   // u8 cmd, u8 ok, u8 code -- response to a MSG_CMD
  MSG_INPUT       = 0x15,   // a physical button event: u8 button(LinkBtn), u8 edge(LinkEdge)
  MSG_CAP_INFO    = 0x16,   // capture-session / SD info snapshot (LinkCapInfo)
  MSG_CAP_ITEM    = 0x17,   // one entry in a capture file list (u8 idx, u8 total, char[16] name, u32 bytes)
  MSG_CAP_REC     = 0x18,   // one record from a capture file (LinkCapRec) for the viewer
  MSG_AUDIO       = 0x19,   // optional: demod audio block, s16 PCM (future; display may ignore)
  MSG_LOG         = 0x1A,   // a short ASCII status line for the UI log/toast (NUL-terminated)
};

// ---- control commands (MSG_CMD payload = u8 cmd, then command-specific bytes) ----
enum LinkCmd : uint8_t {
  CMD_SET_MODE     = 0x01,  // u8 mode (LinkMode)
  CMD_SET_FREQ     = 0x02,  // u32 hz  -- tuner centre frequency
  CMD_SET_SR       = 0x03,  // u32 hz  -- sample rate (RTL valid ranges enforced on Teensy)
  CMD_SET_GAIN     = 0x04,  // u8 autoFlag, i16 tenthdB (gain*10; ignored when auto)
  CMD_SET_PPM      = 0x05,  // i16 ppm
  CMD_SET_BIAST    = 0x06,  // u8 on
  CMD_SET_DIRECT   = 0x07,  // u8 mode (0 off, 1 I-branch, 2 Q-branch)
  CMD_SET_FFT      = 0x08,  // u8 log2N (8..11 => 256..2048), u8 window, u8 avg
  CMD_SET_SWEEP    = 0x09,  // u32 startHz, u32 stopHz, u32 stepHz
  CMD_TUNE_STEP    = 0x0A,  // i32 deltaHz -- relative retune (button coarse/fine)
  CMD_CAP_START    = 0x0B,  // u8 kind (0 events, 1 IQ snapshot)
  CMD_CAP_STOP     = 0x0C,
  CMD_CAP_SAVE     = 0x0D,
  CMD_CAP_DISCARD  = 0x0E,
  CMD_CAP_LIST     = 0x0F,  // ask the Teensy to stream MSG_CAP_ITEM for every file
  CMD_CAP_VIEW     = 0x10,  // char[16] name -> Teensy streams MSG_CAP_REC records
  CMD_CAP_DELETE   = 0x11,  // char[16] name
  CMD_SET_SQUELCH  = 0x12,  // i16 tenth-dBFS squelch threshold for demod/detect
  CMD_SET_DEMOD    = 0x13,  // u8 kind (0 off,1 NBFM,2 WBFM,3 AM,4 USB,5 LSB)
  CMD_REBOOT_SDR   = 0x14,  // force a re-enumerate / re-init of the dongle
  CMD_PERSIST      = 0x15,  // u8 save(1)/load(0) settings on the Teensy
};

// ---- data requests (MSG_REQ payload = u8 what) ----
enum LinkReq : uint8_t {
  REQ_TELEMETRY = 0x01,     // send one MSG_TELEMETRY now
  REQ_HELLO     = 0x02,     // resend MSG_HELLO_TEENSY (display just booted)
};

// ---- operating modes (mirror of the Teensy engine's mode set) ----
enum LinkMode : uint8_t {
  LMODE_IDLE   = 0,
  LMODE_SPECTRUM,           // live FFT trace + waterfall at the tuned centre
  LMODE_SWEEP,              // step across a band, log energy per sub-band
  LMODE_DETECT,             // watch the tuned span, flag peaks above the floor
  LMODE_DEMOD,              // NBFM/WBFM/AM/SSB the tuned channel (audio level/meter)
  LMODE_COUNT
};

// ---- button ids (physical, on the Teensy) ----
enum LinkBtn : uint8_t {
  BTN_UP = 0, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_OK, BTN_BACK, LINKBTN_COUNT
};
enum LinkEdge : uint8_t { EDGE_DOWN = 0, EDGE_UP = 1, EDGE_REPEAT = 2 };

// ============================================================ packed payloads
// All multi-byte fields little-endian; use the put/get helpers so endianness is
// explicit and identical on both CPUs regardless of native order.

static inline void lp_put16(uint8_t *p, uint16_t v){ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static inline void lp_put32(uint8_t *p, uint32_t v){ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
static inline uint16_t lp_get16(const uint8_t *p){ return (uint16_t)(p[0] | (p[1]<<8)); }
static inline uint32_t lp_get32(const uint8_t *p){ return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }
static inline void lp_puti16(uint8_t *p, int16_t v){ lp_put16(p,(uint16_t)v); }
static inline int16_t lp_geti16(const uint8_t *p){ return (int16_t)lp_get16(p); }
static inline void lp_puti32(uint8_t *p, int32_t v){ lp_put32(p,(uint32_t)v); }
static inline int32_t lp_geti32(const uint8_t *p){ return (int32_t)lp_get32(p); }

// MSG_TELEMETRY payload layout (offsets in bytes):
//   0  u32  tunedHz
//   4  u32  sampleRateHz
//   8  u8   mode (LinkMode)
//   9  u8   gainAuto (0/1)
//   10 i16  gainTenthDb
//   12 i16  ppm
//   14 u8   biasTee
//   15 u8   directSamp
//   16 u8   demod (LinkCmd CMD_SET_DEMOD kind)
//   17 u8   tunerType (0 unknown,1 R820T,2 R820T2,3 R860,4 FC0012,5 E4000)
//   18 u8   sdrPresent  | 19 u8 sdrStreaming
//   20 i16  rssiTenthDbfs (wideband power)   | 22 i16 floorTenthDbfs (noise floor)
//   24 u16  busMv (INA219, mV)               | 26 i16 curMa (mA)   | 28 u16 powCw (centi-W)
//   30 i16  dieTenthC (Teensy die temp *10)
//   32 u8   fanDuty
//   33 u8   sdOk  | 34 u8 capActive | 35 u8 capKind
//   36 u32  capCount (records held) | 40 u32 capBytes (RAM used)
//   44 u32  sdFreeMB | 48 u32 sdTotalMB
//   ---- GPS (GT-U7) ----
//   52 u8   gpsValid | 53 u8 gpsSats
//   54 i32  latE7 (lat * 1e7) | 58 i32 lonE7 | 62 i16 altM
//   64 u8   hh | 65 u8 mm | 66 u8 ss   (UTC)
#define TLM_LEN 67u

// MSG_SPECTRUM payload:
//   0  u32 centreHz
//   4  u32 spanHz          (== sample rate for a single-tune spectrum)
//   8  u16 nBins           (<= LINK_MAX_PAYLOAD-16)
//   10 i16 refTenthDbfs    (top of scale, dBFS*10) -> maps mag byte 255
//   12 i16 floorTenthDbfs  (bottom of scale)       -> maps mag byte 0
//   14 u8  seqLo           (column counter, for waterfall tear detection)
//   15 u8  reserved
//   16.. nBins bytes, each 0..255 (already scaled to [floor..ref])
#define SPEC_HDR 16u

// MSG_DETECT payload (a discovered signal):
//   0 u32 freqHz | 4 i16 powTenthDbfs | 6 u16 bwKhz | 8 u32 tMs | 12 u8 kind | 13 u8 flags
#define DET_LEN 14u

// MSG_CAP_INFO payload:
//   0 u8 active | 1 u8 kind | 2 u32 count | 6 u32 bytes | 10 u32 seconds
//   14 u8 sdOk | 15 u8 full | 16 u32 sdFreeMB | 20 u32 sdTotalMB | 24 u16 fileCount
#define CAPINFO_LEN 26u

// ============================================================ CRC-16/MCRF4XX
static inline uint16_t lp_crc16(const uint8_t *data, uint32_t len, uint16_t crc /*=0xFFFF*/) {
  for (uint32_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t b = 0; b < 8; b++)
      crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0x8408) : (uint16_t)(crc >> 1);
  }
  return crc;
}

// ============================================================ encoder
// Builds a full frame into out[] (must be >= LINK_MAX_FRAME). Returns total len.
static inline uint32_t lp_encode(uint8_t *out, uint8_t seq, uint8_t flags,
                                 uint8_t chan, uint8_t msg,
                                 const uint8_t *payload, uint16_t plen) {
  if (plen > LINK_MAX_PAYLOAD) plen = LINK_MAX_PAYLOAD;
  out[0] = LINK_START;
  out[1] = (uint8_t)(plen >> 8);
  out[2] = (uint8_t)plen;
  out[3] = seq;
  out[4] = flags;
  out[5] = chan;
  out[6] = msg;
  if (plen && payload) memcpy(&out[7], payload, plen);
  uint16_t crc = lp_crc16(&out[3], (uint32_t)plen + 4u, 0xFFFF);
  out[7 + plen] = (uint8_t)(crc >> 8);
  out[8 + plen] = (uint8_t)crc;
  return 9u + plen;
}

// ============================================================ incremental parser
// Feed bytes one at a time (or in a loop). When a valid CRC-checked frame is
// complete, onFrame(seq,flags,chan,msg,payload,plen) is invoked. Bad CRCs and
// desyncs are dropped silently and the parser resynchronises on the next 0xAA.
struct LinkParser {
  enum { S_START, S_LEN_H, S_LEN_L, S_SEQ, S_FLAGS, S_CHAN, S_MSG, S_PAY, S_CRC_H, S_CRC_L } st;
  uint16_t plen, pidx;
  uint8_t  seq, flags, chan, msg, crch;
  uint8_t  pay[LINK_MAX_PAYLOAD];
  uint32_t goodFrames, crcErrors, resyncs;

  void reset() { st = S_START; plen = pidx = 0; }
  LinkParser() { reset(); goodFrames = crcErrors = resyncs = 0; }

  // Returns true and fills the out-params when a valid frame completes.
  bool feed(uint8_t c, uint8_t &oseq, uint8_t &oflags, uint8_t &ochan,
            uint8_t &omsg, const uint8_t *&opay, uint16_t &oplen) {
    switch (st) {
      case S_START: if (c == LINK_START) st = S_LEN_H; else resyncs++; break;
      case S_LEN_H: plen = (uint16_t)c << 8; st = S_LEN_L; break;
      case S_LEN_L:
        plen |= c;
        if (plen > LINK_MAX_PAYLOAD) { st = S_START; resyncs++; } else st = S_SEQ;
        break;
      case S_SEQ:   seq = c;   st = S_FLAGS; break;
      case S_FLAGS: flags = c; st = S_CHAN;  break;
      case S_CHAN:  chan = c;  st = S_MSG;   break;
      case S_MSG:   msg = c;   pidx = 0; st = plen ? S_PAY : S_CRC_H; break;
      case S_PAY:
        pay[pidx++] = c;
        if (pidx >= plen) st = S_CRC_H;
        break;
      case S_CRC_H: crch = c; st = S_CRC_L; break;
      case S_CRC_L: {
        uint16_t rc = (uint16_t)((crch << 8) | c);
        uint8_t  h4[4] = { seq, flags, chan, msg };
        uint16_t crc = lp_crc16(h4, 4, 0xFFFF);   // running CRC continues over payload,
        crc = lp_crc16(pay, plen, crc);           // identical to one pass over [hdr|payload]
        st = S_START;
        if (crc == rc) {
          goodFrames++;
          oseq = seq; oflags = flags; ochan = chan; omsg = msg;
          opay = pay; oplen = plen;
          return true;
        }
        crcErrors++;
      } break;
      default: st = S_START; break;
    }
    return false;
  }
};
