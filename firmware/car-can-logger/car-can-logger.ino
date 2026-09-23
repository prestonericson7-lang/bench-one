// ============================================================
//  car-can-logger -- Teensy 4.1 passive CAN logger / bus decoder
//  For a 2017 BMW F30 330e (PHEV), but bus-agnostic.
//
//  WHAT IT DOES
//    * Brings both CAN controllers up in HARDWARE LISTEN-ONLY (silent) mode.
//      The controller physically cannot transmit, ACK or error-frame. A bug
//      in this firmware cannot put a single bit on the car's bus.
//    * Timestamps every frame with micros() and appends it to a RAM ring.
//    * Drains the ring to SD in large sequential blocks (never per-frame),
//      because small synchronous writes cause massive write amplification
//      and widen the window where a power cut corrupts the file.
//    * Sleeps with the car: when the bus goes quiet it flushes, closes the
//      file and idles, so it never holds the bus or the card awake.
//    * Keeps a live ID census with a CHANGED-BIT MASK per ID -- the tool that
//      turns an undocumented BMW bus into named signals.
//
//  ON-DISK RECORD (little-endian, append-only):
//    [u32 t_us][u8 bus][u8 lenflags][u32 id][u8 data[len]]
//      lenflags: bits0-3 = dlc, bit4 = extended id, bit5 = remote frame
//  File header: "CANLOG2\n" then [u32 bus1_baud][u32 bus2_baud]
// ============================================================
#include <FlexCAN_T4.h>
#include <SD.h>
#include "config.h"

FlexCAN_T4<CAN1, RX_SIZE_256, TX_SIZE_16> can1;
#if ENABLE_BUS2
FlexCAN_T4<CAN2, RX_SIZE_256, TX_SIZE_16> can2;
#endif

// ---------------- capture ring (ISR producer -> loop consumer) ----------------
DMAMEM static uint8_t s_ring[RING_BYTES];
static volatile uint32_t s_head = 0;   // written by ISR
static volatile uint32_t s_tail = 0;   // written by loop
static volatile uint32_t s_dropped = 0;

static inline uint32_t ringUsed() {
  uint32_t h = s_head, t = s_tail;
  return (h >= t) ? (h - t) : (RING_BYTES - t + h);
}
static inline uint32_t ringFree() { return RING_BYTES - ringUsed() - 1; }

// ---------------- ID census ----------------
struct IdEntry {
  uint32_t id;
  uint32_t count;
  uint32_t lastMs;
  uint8_t  bus;
  uint8_t  len;
  uint8_t  last[8];
  uint8_t  changed[8];   // OR of every XOR delta seen == which bits are live
  bool     seen;
};
static IdEntry s_ids[ID_TABLE_MAX];
static uint16_t s_idCount = 0;

// ---------------- state ----------------
static bool     s_sdOK      = false;
static File     s_log;
static char     s_logName[20] = {0};
static bool     s_logOpen   = false;
static bool     s_capture   = true;      // armed by default
// Live raw stream over USB, for the Pi-side bridge that turns frames into named
// signals (see canlog.py / can_bridge.py). Off by default: at a few thousand
// frames/s this is a lot of text, and it must never compete with the SD writer
// for time when we are actually logging a drive.
static bool     s_stream    = false;
static uint32_t s_frames[2] = {0, 0};    // per-bus totals
static uint32_t s_lastFrameMs = 0;
static uint32_t s_bytesWritten = 0;
static uint32_t s_ringHigh = 0;          // high-water mark

// ============================================================
//  SD
// ============================================================
static bool sdBegin() {
  for (int i = 0; i < 6; i++) {                       // default DMA_SDIO
    if (SD.begin(SD_CS)) return true;
    delay(120);
  }
  for (int i = 0; i < 4; i++) {                       // FIFO is more tolerant
    if (SD.sdfs.begin(SdioConfig(FIFO_SDIO))) return true;
    delay(120);
  }
  return false;
}

static bool logOpen() {
  if (!s_sdOK) return false;
  for (int i = 1; i < 10000; i++) {
    snprintf(s_logName, sizeof(s_logName), "CAN_%04d.BIN", i);
    if (!SD.exists(s_logName)) break;
  }
  s_log = SD.open(s_logName, FILE_WRITE);
  if (!s_log) return false;
  uint32_t b1 = BUS1_BAUD, b2 = ENABLE_BUS2 ? BUS2_BAUD : 0;
  s_log.write((const uint8_t*)LOG_MAGIC, 8);
  s_log.write((uint8_t*)&b1, 4);
  s_log.write((uint8_t*)&b2, 4);
  s_bytesWritten = 16;
  s_logOpen = true;
  Serial.printf("log open: %s\n", s_logName);
  return true;
}

static void logClose() {
  if (!s_logOpen) return;
  s_log.flush(); s_log.close();
  s_logOpen = false;
  Serial.printf("log closed: %s (%lu bytes)\n", s_logName, (unsigned long)s_bytesWritten);
}

// ============================================================
//  RX -- runs in interrupt context. Do the minimum: stamp, pack, push.
// ============================================================
static void onFrame(const CAN_message_t &m) {
  if (!s_capture) return;
  uint32_t t = micros();
  uint8_t len = m.len > 8 ? 8 : m.len;
  uint32_t need = 10u + len;

  if (ringFree() < need) { s_dropped++; return; }

  uint8_t rec[18];
  memcpy(rec + 0, &t, 4);
  rec[4] = m.bus;
  rec[5] = (uint8_t)((len & 0x0F) | (m.flags.extended ? 0x10 : 0) | (m.flags.remote ? 0x20 : 0));
  memcpy(rec + 6, &m.id, 4);
  memcpy(rec + 10, m.buf, len);

  uint32_t h = s_head;
  for (uint32_t i = 0; i < need; i++) {
    s_ring[h] = rec[i];
    if (++h >= RING_BYTES) h = 0;
  }
  s_head = h;

  uint8_t b = (m.bus <= 1) ? m.bus : 1;
  s_frames[b]++;
  s_lastFrameMs = millis();

  // Live text stream: "R,<t_us>,<bus>,<id hex>,<data hex>"
  if (s_stream) {
    Serial.print("R,"); Serial.print(t); Serial.print(',');
    Serial.print(m.bus); Serial.print(',');
    Serial.print(m.id, HEX); Serial.print(',');
    for (uint8_t i = 0; i < len; i++) {
      if (m.buf[i] < 16) Serial.print('0');
      Serial.print(m.buf[i], HEX);
    }
    Serial.println();
  }

  // ---- ID census + changed-bit mask ----
  IdEntry *e = nullptr;
  for (uint16_t i = 0; i < s_idCount; i++) {
    if (s_ids[i].id == m.id && s_ids[i].bus == m.bus) { e = &s_ids[i]; break; }
  }
  if (!e && s_idCount < ID_TABLE_MAX) {
    e = &s_ids[s_idCount++];
    e->id = m.id; e->bus = m.bus; e->count = 0; e->seen = false;
    memset(e->changed, 0, 8); memset(e->last, 0, 8);
  }
  if (e) {
    if (e->seen) {
      for (uint8_t i = 0; i < len; i++) e->changed[i] |= (e->last[i] ^ m.buf[i]);
    }
    memcpy(e->last, m.buf, len);
    e->len = len; e->count++; e->lastMs = millis(); e->seen = true;
  }
}

// ============================================================
//  drain ring -> SD in big blocks
// ============================================================
static void drainToSD(bool force) {
  if (!s_logOpen) return;
  uint32_t used = ringUsed();
  if (used > s_ringHigh) s_ringHigh = used;
  if (!force && used < SD_BLOCK_BYTES) return;

  while (used > 0) {
    uint32_t t = s_tail;
    uint32_t chunk = (used > SD_BLOCK_BYTES) ? SD_BLOCK_BYTES : used;
    uint32_t toEnd = RING_BYTES - t;
    if (chunk > toEnd) chunk = toEnd;          // one contiguous span at a time
    s_log.write(&s_ring[t], chunk);
    s_bytesWritten += chunk;
    t += chunk; if (t >= RING_BYTES) t = 0;
    s_tail = t;
    used = ringUsed();
    if (!force && used < SD_BLOCK_BYTES) break;
  }
}

// ============================================================
//  console
// ============================================================
static void printStats() {
  Serial.printf("STATUS cap=%s sd=%s log=%s bus1=%lu bus2=%lu ids=%u dropped=%lu ring=%lu/%lu high=%lu written=%luKB quiet=%lums\n",
    s_capture ? "ON" : "OFF", s_sdOK ? "ok" : "NODET",
    s_logOpen ? s_logName : "(closed)",
    (unsigned long)s_frames[0], (unsigned long)s_frames[1], s_idCount,
    (unsigned long)s_dropped, (unsigned long)ringUsed(), (unsigned long)RING_BYTES,
    (unsigned long)s_ringHigh, (unsigned long)(s_bytesWritten / 1024),
    (unsigned long)(millis() - s_lastFrameMs));
}

// The decoder view: every ID, how fast it repeats, and which bits are alive.
static void printCensus() {
  Serial.printf("-- %u IDs seen --\n", s_idCount);
  Serial.println("bus  id      count   rate    live-bits (bit set = that bit has changed)");
  for (uint16_t i = 0; i < s_idCount; i++) {
    IdEntry &e = s_ids[i];
    float secs = millis() / 1000.0f;
    Serial.printf("%u    %03lX   %6lu  %5.1f/s  ",
      e.bus, (unsigned long)e.id, (unsigned long)e.count, secs > 0 ? e.count / secs : 0);
    for (uint8_t b = 0; b < e.len; b++) Serial.printf("%02X ", e.changed[b]);
    Serial.print("  last: ");
    for (uint8_t b = 0; b < e.len; b++) Serial.printf("%02X ", e.last[b]);
    Serial.println();
  }
}

static void handleUSB() {
  while (Serial.available()) {
    char c = Serial.read();
    if      (c == 'i') printStats();
    else if (c == 'd') printCensus();
    else if (c == 'c') { s_idCount = 0; Serial.println("census cleared"); }
    else if (c == 's') { s_capture = !s_capture; Serial.printf("capture %s\n", s_capture ? "ON" : "OFF"); }
    else if (c == 'f') { drainToSD(true); if (s_logOpen) s_log.flush(); Serial.println("flushed"); }
    else if (c == 'n') { drainToSD(true); logClose(); logOpen(); }
    else if (c == 'm') { s_sdOK = sdBegin(); Serial.printf("SD %s\n", s_sdOK ? "ok" : "FAIL"); }
    else if (c == 'r') { s_stream = !s_stream; Serial.printf("raw stream %s\n", s_stream ? "ON" : "OFF"); }
    else if (c == '?') Serial.println("i=status d=census c=clear-census s=start/stop f=flush n=new-file m=remount-sd r=raw-stream");
  }
}

// ============================================================
void setup() {
  Serial.begin(115200);

  can1.begin();
  can1.setBaudRate(BUS1_BAUD, LISTEN_ONLY);   // hardware silent: cannot TX/ACK
  can1.setMaxMB(16);
  can1.enableFIFO();
  can1.enableFIFOInterrupt();
  can1.onReceive(onFrame);

#if ENABLE_BUS2
  can2.begin();
  can2.setBaudRate(BUS2_BAUD, LISTEN_ONLY);
  can2.setMaxMB(16);
  can2.enableFIFO();
  can2.enableFIFOInterrupt();
  can2.onReceive(onFrame);
#endif

  s_sdOK = sdBegin();
  if (s_sdOK) logOpen();
  s_lastFrameMs = millis();

  Serial.println("car-can-logger: LISTEN-ONLY on both buses (cannot transmit). '?' for commands.");
  Serial.printf("bus1=%lu baud  bus2=%s\n", (unsigned long)BUS1_BAUD,
                ENABLE_BUS2 ? String((unsigned long)BUS2_BAUD).c_str() : "disabled");
}

void loop() {
  can1.events();
#if ENABLE_BUS2
  can2.events();
#endif
  handleUSB();

  uint32_t quiet = millis() - s_lastFrameMs;

  drainToSD(false);                              // normal: only full blocks

  if (quiet > IDLE_FLUSH_MS && s_logOpen && ringUsed() > 0) {
    drainToSD(true);                             // bus paused -> get it on disk
    s_log.flush();
  }
  if (quiet > IDLE_SLEEP_MS && s_logOpen) {
    drainToSD(true);
    logClose();                                  // sleep with the car
    Serial.println("bus idle -> log closed, idling");
  }
  if (quiet < IDLE_FLUSH_MS && !s_logOpen && s_sdOK && s_capture) {
    logOpen();                                   // traffic returned -> new file
  }

  static uint32_t lastPrint = 0;
  if (millis() - lastPrint > 5000) { lastPrint = millis(); printStats(); }
}
