/* ===========================================================================================
 *  rtlsdr-display.ino -- E32R40T (ESP32-WROOM-32E) : the UI computer
 * ===========================================================================================
 *
 *  Board: LCDWIKI E32R40T. ESP32-WROOM-32E (the CLASSIC ESP32, not an S3),
 *         4.0" ST7796 480x320 + XPT2046 resistive touch (TOUCH IS DEAD on this
 *         unit, so it is not used -- all input is physical buttons on the Teensy,
 *         forwarded to us as MSG_INPUT events).
 *
 *  ROLE: this node RUNS the UI. It owns the menu/screen state machine and every
 *  pixel of layout + the waterfall, so the Teensy stays free for the SDR. It
 *  consumes semantic DATA (spectrum / detections / telemetry) and button events,
 *  and sends SDR CONTROL COMMANDS back down.
 *
 *  LINK: UART2 on P4 ("I2C" connector): GPIO25 = TX (-> Teensy RX7 pin 28),
 *        GPIO32 = RX (<- Teensy TX7 pin 29), GND on P4.4. 921600 8N1.
 *        Do NOT use P2 ("UART") -- it is UART0 in parallel with the CH340C.
 *
 *  Display backend: TFT_eSPI, configured entirely by build_opt.h in this folder
 *  (no edit to any installed library). Arduino_GFX does not build against the
 *  installed esp32 core, which is why TFT_eSPI is used here.
 * =========================================================================================== */
#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include "link_proto.h"

// ---- identity + FFT-size bounds echoed from the Teensy engine's config.h ----
#define FW_MAJOR 1
#define FW_MINOR 0
#define SDR_DEF_FFT_LOG2 10
#define SDR_MIN_FFT_LOG2 8
#define SDR_MAX_FFT_LOG2 11

// ---- board pins (E32R40T, from docs/research/FINDINGS.md) ----
#define PIN_LCD_BL     27
#define PIN_LED_R      22     // RGB LED common anode: HIGH = off
#define PIN_LED_G      16
#define PIN_LED_B      17
#define PIN_AUDIO_EN    4     // active LOW
#define LINK_TX        25     // UART2 TX -> Teensy RX
#define LINK_RX        32     // UART2 RX <- Teensy TX
#define LINK_UART_NUM   2

// ---- geometry (landscape) ----
#define SCR_W 480
#define SCR_H 320
#define TOPBAR_H  22
#define HUD_Y     24
#define HUD_H     20
#define TRACE_Y   48
#define TRACE_H   96
#define WF_Y      146
#define WF_H      150
#define FOOT_Y    300

// ---- colours (RGB565) ----
#define C_BG    0x0000
#define C_INK   0xFFFF
#define C_DIM   0x8410
#define C_ACC   0x05FF
#define C_OK    0x07E0
#define C_WARN  0xFD20
#define C_BAD   0xF800
#define C_RULE  0x2124
#define C_SELBG 0x001F
#define C_GRID  0x18E3

static TFT_eSPI    tft = TFT_eSPI();
static TFT_eSprite wf  = TFT_eSprite(&tft);
static bool        wfReady = false;
static uint8_t     wfLut8[256];   // magnitude -> RRRGGGBB (332) for the 8-bit sprite

static HardwareSerial LinkSerial(LINK_UART_NUM);
static LinkParser parser;
static uint8_t  seq = 0;
static uint8_t  txbuf[LINK_MAX_FRAME];
static uint32_t lastRxMs = 0;
static uint32_t lastHelloMs = 0;

// ======================================================================= screens
enum Screen {
  SCR_MENU = 0, SCR_SPECTRUM, SCR_BANDS, SCR_SWEEP, SCR_DETECT, SCR_DEMOD,
  SCR_CONFIG, SCR_CAPTURES, SCR_SYSTEM, SCR_SETTINGS, SCR_COUNT
};
static const char* SCREEN_NAMES[SCR_COUNT] = {
  "MENU","SPECTRUM","BANDS","SWEEP","DETECT","DEMOD","CONFIG","CAPTURES","SYSTEM","SETTINGS"
};
static Screen scr = SCR_MENU;
static bool   needFull = true;

// menu entries (screen id + optional mode to request)
struct MenuItem { const char* label; Screen target; int mode; };
static const MenuItem MENU[] = {
  { "Spectrum",  SCR_SPECTRUM, LMODE_SPECTRUM },
  { "Bands",     SCR_BANDS,    -1 },
  { "Sweep",     SCR_SWEEP,    LMODE_SWEEP },
  { "Detect",    SCR_DETECT,   LMODE_DETECT },
  { "Demod",     SCR_DEMOD,    LMODE_DEMOD },
  { "Config",    SCR_CONFIG,   -1 },
  { "Captures",  SCR_CAPTURES, -1 },
  { "System",    SCR_SYSTEM,   -1 },
  { "Settings",  SCR_SETTINGS, -1 },
};
static const int MENU_N = sizeof(MENU)/sizeof(MENU[0]);
static int menuSel = 0;

// ======================================================================= data caches
struct Telemetry {
  uint32_t tunedHz, sampleRateHz;
  uint8_t  mode, gainAuto;
  int16_t  gainTenthDb, ppm;
  uint8_t  biasTee, directSamp, demod, tunerType, sdrPresent, sdrStreaming;
  int16_t  rssiTenthDbfs, floorTenthDbfs;
  uint16_t busMv; int16_t curMa; uint16_t powCw;
  int16_t  dieTenthC;
  uint8_t  fanDuty, sdOk, capActive, capKind;
  uint32_t capCount, capBytes, sdFreeMB, sdTotalMB;
  uint8_t  gpsValid, gpsSats;
  int32_t  latE7, lonE7;
  int16_t  altM;
  uint8_t  hh, mm, ss;
  bool     valid;
} tlm;

static uint8_t  fwMajor = 0, fwMinor = 0;

// spectrum
static uint8_t  specMag[512];
static uint16_t specBins = 0;
static uint32_t specCentre = 0, specSpan = 0;
static int16_t  specRefT = 0, specFloorT = -800;
static bool     specNew = false;

// detections list
struct Det { uint32_t freqHz; int16_t powT; uint16_t bwKhz; uint32_t tMs; uint8_t kind; };
#define DET_MAX 32
static Det   dets[DET_MAX];
static int   detN = 0, detHead = 0;

// captures browser
struct CapItem { char name[16]; uint32_t bytes; };
#define CAP_MAX 48
#define CAP_ACTIONS 2                 // the two capture-control rows above the file list
static CapItem caps[CAP_MAX];
static int  capN = 0, capSel = 0;
static bool capListPending = false;

// capture record viewer
struct CapRec { uint32_t freqHz; int16_t powT; uint16_t bwKhz; uint32_t tMs; uint8_t kind; };
#define CAPREC_MAX 64
static CapRec capRecs[CAPREC_MAX];
static int  capRecN = 0, capRecSel = 0;
static bool inCapView = false;

// log/toast
static char toast[40] = {0};
static uint32_t toastMs = 0;

// tuning step
static const uint32_t STEPS[] = { 1000, 5000, 10000, 100000, 1000000, 10000000 };
static const int STEP_N = sizeof(STEPS)/sizeof(STEPS[0]);
static int stepIdx = 3;   // 100 kHz

// backlight
static uint8_t backlight = 200;

// bands (mirror of the Teensy presets, for local selection UI)
struct Band { const char* name; uint32_t hz; uint32_t sr; uint8_t demod; };
static const Band BANDS[] = {
  { "FM Broadcast", 100100000UL, 2400000UL, 2 },
  { "NOAA Wx 1",    162400000UL,  250000UL, 1 },
  { "Airband AM",   124000000UL,  250000UL, 3 },
  { "2m Ham",       145000000UL,  250000UL, 1 },
  { "70cm Ham",     435000000UL,  250000UL, 1 },
  { "ISM 433",      433920000UL, 1024000UL, 0 },
  { "ISM 915",      915000000UL, 2400000UL, 0 },
  { "ADS-B 1090",  1090000000UL, 2400000UL, 0 },
  { "Marine VHF",   156800000UL,  250000UL, 1 },
  { "PMR446",       446000000UL,  250000UL, 1 },
};
static const int BAND_N = sizeof(BANDS)/sizeof(BANDS[0]);
static int bandSel = 0;

// config editor
enum CfgField { CF_FREQ=0, CF_SR, CF_GAINMODE, CF_GAIN, CF_PPM, CF_BIAS, CF_DIRECT, CF_FFT,
                CF_SQUELCH, CF_DEMOD, CF_SWSTART, CF_SWSTOP, CF_SWSTEP, CF_COUNT };
static int cfgSel = 0;
static int cfgFft = SDR_DEF_FFT_LOG2;   // local echo, applied on change
static int cfgSquelchT = -350;
// sweep range (display owns these; sent to the Teensy on change)
static uint32_t cfgSwStart = 400000000UL, cfgSwStop = 900000000UL, cfgSwStep = 2000000UL;
static const char* DEMOD_NAMES[] = { "Off","NBFM","WBFM","AM","USB","LSB" };

// settings editor
enum SetField { SF_BL=0, SF_AVG, SF_REINIT, SF_COUNT };
static int setSel = 0;
static int setAvg = 2;

// ======================================================================= link TX
static void linkSend(uint8_t msg, const uint8_t* pay, uint16_t plen, uint8_t flags=0) {
  uint32_t n = lp_encode(txbuf, seq++, flags, LINK_CHAN_DISPLAY, msg, pay, plen);
  LinkSerial.write(txbuf, n);
}
static void sendCmd(uint8_t cmd, const uint8_t* a=nullptr, uint16_t alen=0) {
  uint8_t p[64]; if (alen > 63) alen = 63; p[0]=cmd; if (alen && a) memcpy(&p[1], a, alen);
  linkSend(MSG_CMD, p, (uint16_t)(1+alen));
}
static void sendHelloDisp() {
  uint8_t p[4] = { 1 /*proto*/, (uint8_t)esp_reset_reason(), 1 /*panelOk*/, 0 /*touchOk=DEAD*/ };
  linkSend(MSG_HELLO_DISP, p, 4);
}
static void sendReq(uint8_t what) { uint8_t p=what; linkSend(MSG_REQ, &p, 1); }
static void cmdU32(uint8_t cmd, uint32_t v){ uint8_t a[4]; lp_put32(a,v); sendCmd(cmd,a,4); }
static void cmdI32(uint8_t cmd, int32_t v){ uint8_t a[4]; lp_puti32(a,v); sendCmd(cmd,a,4); }
static void cmdI16(uint8_t cmd, int16_t v){ uint8_t a[2]; lp_puti16(a,v); sendCmd(cmd,a,2); }
static void cmdU8(uint8_t cmd, uint8_t v){ sendCmd(cmd,&v,1); }
static void sendSetGain(bool autoG, int16_t tdb){ uint8_t a[3]; a[0]=autoG?1:0; lp_puti16(&a[1],tdb); sendCmd(CMD_SET_GAIN,a,3); }
static void sendSetFft(uint8_t log2n, uint8_t win, uint8_t avg){ uint8_t a[3]={log2n,win,avg}; sendCmd(CMD_SET_FFT,a,3); }
static void tuneToBand(int i) {
  cmdU32(CMD_SET_SR, BANDS[i].sr);
  cmdU32(CMD_SET_FREQ, BANDS[i].hz);
  cmdU8(CMD_SET_DEMOD, BANDS[i].demod);
}

// ======================================================================= palette / waterfall
static void heatRGB(uint8_t v, uint8_t& r, uint8_t& g, uint8_t& b) {
  // black -> blue -> cyan -> green -> yellow -> red -> white
  if      (v <  32) { r=0;            g=0;            b=(uint8_t)(v*4); }
  else if (v <  96) { r=0;            g=(uint8_t)((v-32)*4); b=255; }
  else if (v < 160) { r=0;            g=255;          b=(uint8_t)(255-(v-96)*4); }
  else if (v < 208) { r=(uint8_t)((v-160)*5); g=255;  b=0; }
  else if (v < 240) { r=255;          g=(uint8_t)(255-(v-208)*8); b=0; }
  else              { r=255;          g=(uint8_t)((v-240)*16); b=(uint8_t)((v-240)*16); }
}
// build the magnitude -> RRRGGGBB (332) lookup for the 8-bit waterfall sprite
static void buildPalette() {
  for (int i=0;i<256;i++){ uint8_t r,g,b; heatRGB((uint8_t)i,r,g,b);
    wfLut8[i] = (uint8_t)((r & 0xE0) | ((g & 0xE0) >> 3) | (b >> 6)); }
}

static void wfInit() {
  wf.setColorDepth(8);   // 8-bit RRRGGGBB; scroll() works (unlike 4-bit palette)
  if (wf.createSprite(SCR_W, WF_H)) {
    wf.fillSprite(0);
    wfReady = true;
  } else {
    wfReady = false;   // not enough heap -> waterfall disabled, trace still works
  }
}

// draw one incoming spectrum column into the waterfall
static void wfPush() {
  if (!wfReady) return;
  wf.scroll(0, 1);                     // shift down by one row
  for (int x = 0; x < SCR_W; x++) {
    int bin = (specBins > 0) ? (x * specBins / SCR_W) : 0;
    if (bin >= specBins) bin = specBins - 1;
    wf.drawPixel(x, 0, wfLut8[specMag[bin]]);   // 8-bit 332 heat colour
  }
  wf.pushSprite(0, WF_Y);
}

// ======================================================================= drawing helpers
static void drawTopBar() {
  tft.fillRect(0, 0, SCR_W, TOPBAR_H, C_BG);
  tft.setTextFont(2); tft.setTextColor(C_INK, C_BG);
  tft.setCursor(6, 3); tft.print("RTL-SDR PENTEST");
  tft.setTextColor(C_DIM, C_BG);
  tft.setCursor(200, 3); tft.print(SCREEN_NAMES[scr]);
  bool up = (millis() - lastRxMs) < LINK_TIMEOUT_MS && lastRxMs != 0;
  tft.setTextColor(up ? C_OK : C_BAD, C_BG);
  tft.setCursor(410, 3); tft.print(up ? "LINK UP" : "LINK--");
  tft.drawFastHLine(0, TOPBAR_H, SCR_W, C_RULE);
}

static void fmtHz(uint32_t hz, char* out) {
  double m = hz / 1e6;
  snprintf(out, 20, "%.4f MHz", m);
}
static void fmtHzShort(uint32_t hz, char* out) {
  if (hz >= 1000000) snprintf(out, 16, "%.3fM", hz/1e6);
  else if (hz >= 1000) snprintf(out, 16, "%.1fk", hz/1e3);
  else snprintf(out, 16, "%luHz", (unsigned long)hz);
}

static void drawHud() {
  tft.fillRect(0, HUD_Y, SCR_W, HUD_H, C_BG);
  tft.setTextFont(2);
  char b[24];
  tft.setTextColor(C_ACC, C_BG); tft.setCursor(6, HUD_Y+2);
  fmtHz(tlm.tunedHz, b); tft.print(b);
  tft.setTextColor(C_DIM, C_BG); tft.setCursor(170, HUD_Y+2);
  fmtHzShort(tlm.sampleRateHz, b); tft.printf("SR %s", b);
  tft.setCursor(280, HUD_Y+2);
  if (tlm.gainAuto) tft.print("AGC");
  else { tft.printf("%d.%ddB", tlm.gainTenthDb/10, abs(tlm.gainTenthDb%10)); }
  tft.setCursor(360, HUD_Y+2);
  const char* src = (tlm.tunerType==0xFF) ? "SIM" : (tlm.sdrPresent ? "RTL" : "no dev");
  tft.setTextColor(tlm.sdrStreaming ? C_OK : C_WARN, C_BG);
  tft.print(src);
  tft.drawFastHLine(0, HUD_Y+HUD_H, SCR_W, C_RULE);
}

static void drawTrace() {
  tft.fillRect(0, TRACE_Y, SCR_W, TRACE_H, C_BG);
  // grid
  for (int i = 1; i < 4; i++) tft.drawFastHLine(0, TRACE_Y + i*TRACE_H/4, SCR_W, C_GRID);
  tft.drawFastVLine(SCR_W/2, TRACE_Y, TRACE_H, C_GRID);   // centre
  // trace
  int lastY = -1;
  for (int x = 0; x < SCR_W; x++) {
    int bin = (specBins > 0) ? (x * specBins / SCR_W) : 0;
    if (bin >= specBins) bin = specBins - 1;
    int v = specMag[bin];                       // 0..255
    int y = TRACE_Y + TRACE_H - 1 - (v * (TRACE_H-1) / 255);
    if (lastY < 0) lastY = y;
    tft.drawLine(x-1 < 0 ? 0 : x-1, lastY, x, y, C_ACC);
    lastY = y;
  }
  tft.drawFastHLine(0, TRACE_Y+TRACE_H, SCR_W, C_RULE);
}

static void drawFooter(const char* hint) {
  tft.fillRect(0, FOOT_Y, SCR_W, SCR_H-FOOT_Y, C_BG);
  tft.drawFastHLine(0, FOOT_Y, SCR_W, C_RULE);
  tft.setTextFont(2); tft.setTextColor(C_DIM, C_BG);
  tft.setCursor(6, FOOT_Y+3);
  if (toast[0] && (millis()-toastMs < 2500)) { tft.setTextColor(C_OK,C_BG); tft.print(toast); }
  else tft.print(hint);
}

// ======================================================================= per-screen render
static void renderMenu(bool full) {
  if (full) { tft.fillScreen(C_BG); drawTopBar(); }
  int y0 = 40, rh = 26;
  for (int i = 0; i < MENU_N; i++) {
    int y = y0 + i*rh;
    tft.fillRect(0, y, SCR_W, rh, i==menuSel ? C_SELBG : C_BG);
    tft.setTextFont(4);
    tft.setTextColor(i==menuSel ? C_INK : C_DIM, i==menuSel ? C_SELBG : C_BG);
    tft.setCursor(20, y+3); tft.print(MENU[i].label);
  }
  drawFooter("UP/DN select   OK enter");
}

static void renderSpectrumLike(bool full, const char* hint) {
  if (full) { tft.fillScreen(C_BG); }
  drawTopBar(); drawHud();
  drawTrace();
  if (full && wfReady) wf.pushSprite(0, WF_Y);
  drawFooter(hint);
}

static void renderBands(bool full) {
  if (full) { tft.fillScreen(C_BG); drawTopBar(); }
  int y0=32, rh=25;
  for (int i=0;i<BAND_N;i++){
    int y=y0+i*rh;
    tft.fillRect(0,y,SCR_W,rh, i==bandSel?C_SELBG:C_BG);
    tft.setTextFont(2);
    tft.setTextColor(i==bandSel?C_INK:C_DIM, i==bandSel?C_SELBG:C_BG);
    tft.setCursor(16,y+4);
    char fb[20]; fmtHz(BANDS[i].hz, fb);
    tft.printf("%-14s %s", BANDS[i].name, fb);
  }
  drawFooter("UP/DN select   OK tune   BACK menu");
}

static void renderDetList(int y0, int h) {
  tft.fillRect(0, y0, SCR_W, h, C_BG);
  tft.setTextFont(2); tft.setTextColor(C_DIM,C_BG);
  tft.setCursor(6,y0); tft.print("freq        pow    bw     age");
  int rows = (h-18)/16; if (rows>DET_MAX) rows=DET_MAX;
  for (int i=0;i<rows && i<detN;i++){
    int idx = (detHead - 1 - i + DET_MAX) % DET_MAX;
    Det& d = dets[idx];
    int y = y0 + 18 + i*16;
    tft.setTextColor(C_INK,C_BG); tft.setCursor(6,y);
    char fb[20]; fmtHz(d.freqHz, fb);
    tft.printf("%-11s %3d.%d %4uk", fb, d.powT/10, abs(d.powT%10), d.bwKhz);
  }
}

static void renderDetect(bool full) {
  if (full) tft.fillScreen(C_BG);
  drawTopBar(); drawHud(); drawTrace();
  if (full && wfReady) { /* skip wf; detect uses list below */ }
  renderDetList(WF_Y, WF_H);
  drawFooter("L/R tune  U/D step  OK cfg  BACK menu");
}

static void renderSweep(bool full) {
  if (full) tft.fillScreen(C_BG);
  drawTopBar(); drawHud(); drawTrace();
  renderDetList(WF_Y, WF_H);
  drawFooter("sweeping...  OK cfg   BACK menu");
}

static void renderDemod(bool full) {
  if (full) tft.fillScreen(C_BG);
  drawTopBar(); drawHud();
  // big level meter from rssiTenthDbfs mapped 0..1
  tft.fillRect(0, TRACE_Y, SCR_W, TRACE_H+WF_H, C_BG);
  float lvl = (tlm.rssiTenthDbfs/10.0f + 100.0f) / 100.0f;
  if (lvl<0)lvl=0; if(lvl>1)lvl=1;
  int barw = (int)(lvl*(SCR_W-40));
  tft.drawRect(20, 120, SCR_W-40, 40, C_DIM);
  tft.fillRect(22, 122, barw, 36, lvl>0.8?C_BAD:(lvl>0.4?C_WARN:C_OK));
  tft.setTextFont(4); tft.setTextColor(C_INK,C_BG); tft.setCursor(20, 180);
  tft.printf("Demod: %s", DEMOD_NAMES[tlm.demod<6?tlm.demod:0]);
  tft.setCursor(20, 220); tft.printf("Level: %d%%", (int)(lvl*100));
  drawFooter("L/R tune  U/D demod  BACK menu");
}

static void cfgValue(int f, char* out) {
  switch(f){
    case CF_FREQ:   fmtHz(tlm.tunedHz,out); break;
    case CF_SR:     snprintf(out,20,"%.3f MHz", tlm.sampleRateHz/1e6); break;
    case CF_GAINMODE: snprintf(out,20,"%s", tlm.gainAuto?"AGC (auto)":"manual"); break;
    case CF_GAIN:   snprintf(out,20,"%d.%d dB", tlm.gainTenthDb/10, abs(tlm.gainTenthDb%10)); break;
    case CF_PPM:    snprintf(out,20,"%d ppm", tlm.ppm); break;
    case CF_BIAS:   snprintf(out,20,"%s", tlm.biasTee?"ON (4.5V)":"off"); break;
    case CF_DIRECT: snprintf(out,20,"%s", tlm.directSamp==0?"off":(tlm.directSamp==1?"I-branch":"Q-branch")); break;
    case CF_FFT:    snprintf(out,20,"%d pts", 1<<cfgFft); break;
    case CF_SQUELCH:snprintf(out,20,"%d.%d dB", cfgSquelchT/10, abs(cfgSquelchT%10)); break;
    case CF_DEMOD:  snprintf(out,20,"%s", DEMOD_NAMES[tlm.demod<6?tlm.demod:0]); break;
    case CF_SWSTART:snprintf(out,20,"%.1f MHz", cfgSwStart/1e6); break;
    case CF_SWSTOP: snprintf(out,20,"%.1f MHz", cfgSwStop/1e6); break;
    case CF_SWSTEP: snprintf(out,20,"%.2f MHz", cfgSwStep/1e6); break;
    default: out[0]=0;
  }
}
static const char* CFG_LABELS[CF_COUNT] = {
  "Frequency","Sample rate","Gain mode","Gain","PPM corr","Bias tee","Direct samp","FFT size",
  "Squelch","Demod","Sweep start","Sweep stop","Sweep step"
};
static void sendSetSweep() {
  uint8_t a[12]; lp_put32(&a[0],cfgSwStart); lp_put32(&a[4],cfgSwStop); lp_put32(&a[8],cfgSwStep);
  sendCmd(CMD_SET_SWEEP, a, 12);
}
static void renderConfig(bool full) {
  if (full) { tft.fillScreen(C_BG); drawTopBar(); }
  const int y0=28, rh=21;
  int rows=(FOOT_Y - y0)/rh;                         // fits without hitting the footer
  int first = (cfgSel >= rows) ? (cfgSel - rows + 1) : 0;   // scroll to keep sel visible
  tft.fillRect(0, y0, SCR_W, FOOT_Y - y0, C_BG);
  for (int k=0;k<rows && (first+k)<CF_COUNT;k++){
    int i = first+k;
    int y=y0+k*rh;
    tft.fillRect(0,y,SCR_W,rh, i==cfgSel?C_SELBG:C_BG);
    tft.setTextFont(2);
    tft.setTextColor(i==cfgSel?C_INK:C_DIM, i==cfgSel?C_SELBG:C_BG);
    tft.setCursor(16,y+3); tft.print(CFG_LABELS[i]);
    char v[24]; cfgValue(i,v);
    tft.setCursor(230,y+3); tft.print(v);
  }
  // scroll indicator
  if (CF_COUNT > rows) { tft.setTextColor(C_DIM,C_BG); tft.setCursor(SCR_W-40,y0);
                         tft.printf("%d/%d", cfgSel+1, (int)CF_COUNT); }
  drawFooter("U/D field  L/R change  BACK menu");
}

static void renderSystem(bool full) {
  if (full) { tft.fillScreen(C_BG); }
  drawTopBar();
  tft.fillRect(0,26,SCR_W,SCR_H-26-20,C_BG);
  tft.setTextFont(2); int y=28; const int rh=18;
  auto line=[&](const char* k, const char* v, uint16_t c){
    tft.setTextColor(C_DIM,C_BG); tft.setCursor(16,y); tft.print(k);
    tft.setTextColor(c,C_BG); tft.setCursor(210,y); tft.print(v); y+=rh;
  };
  char b[32];
  const char* src = (tlm.tunerType==0xFF)?"SIM":(tlm.sdrPresent?"RTL2832U":"none");
  line("SDR source", src, tlm.sdrPresent?C_OK:C_WARN);
  const char* tn = tlm.tunerType==0xFF?"SIM":tlm.tunerType==5?"R820T2/R860":tlm.tunerType==6?"R828D":"unknown";
  line("Tuner", tn, C_INK);
  line("Streaming", tlm.sdrStreaming?"yes":"no", tlm.sdrStreaming?C_OK:C_WARN);
  snprintf(b,32,"%.4f MHz", tlm.tunedHz/1e6); line("Tuned", b, C_ACC);
  snprintf(b,32,"%.3f MHz", tlm.sampleRateHz/1e6); line("Sample rate", b, C_INK);
  snprintf(b,32,"%u.%03u V", tlm.busMv/1000, tlm.busMv%1000); line("Bus voltage", b, C_INK);
  snprintf(b,32,"%d mA", tlm.curMa); line("Current", b, C_INK);
  snprintf(b,32,"%u.%02u W", tlm.powCw/100, tlm.powCw%100); line("Power", b, C_INK);
  snprintf(b,32,"%d.%d C", tlm.dieTenthC/10, abs(tlm.dieTenthC%10)); line("Die temp", b, tlm.dieTenthC>700?C_WARN:C_INK);
  snprintf(b,32,"%u%%", (tlm.fanDuty*100)/255); line("Fan", b, C_INK);
  snprintf(b,32,"%s  %lu/%lu MB", tlm.sdOk?"ok":"no card",(unsigned long)tlm.sdFreeMB,(unsigned long)tlm.sdTotalMB);
  line("SD", b, tlm.sdOk?C_OK:C_WARN);
  snprintf(b,32,"fw %u.%u", fwMajor, fwMinor); line("Teensy", b, C_DIM);
  if (tlm.gpsValid) {
    snprintf(b,32,"%u sats, fix", tlm.gpsSats); line("GPS", b, C_OK);
    snprintf(b,32,"%.5f %.5f", tlm.latE7/1e7, tlm.lonE7/1e7); line("Position", b, C_ACC);
    snprintf(b,32,"%02u:%02u:%02u UTC", tlm.hh, tlm.mm, tlm.ss); line("Time", b, C_INK);
  } else {
    snprintf(b,32,"no fix (%u sats)", tlm.gpsSats); line("GPS", b, C_WARN);
  }
  drawFooter("BACK menu");
}

static void renderCaptures(bool full) {
  if (full) { tft.fillScreen(C_BG); drawTopBar(); }
  tft.fillRect(0,26,SCR_W,SCR_H-46,C_BG);
  tft.setTextFont(2);
  if (inCapView) {
    tft.setTextColor(C_ACC,C_BG); tft.setCursor(6,28);
    tft.printf("records: %d", capRecN);
    int rows=(SCR_H-70)/16;
    int first = (capRecSel >= rows) ? (capRecSel - rows + 1) : 0;   // scroll to keep sel visible
    for (int k=0;k<rows && (first+k)<capRecN;k++){
      int i = first+k;
      int y=50+k*16; CapRec& r=capRecs[i];
      tft.fillRect(0,y,SCR_W,16, i==capRecSel?C_SELBG:C_BG);
      tft.setTextColor(i==capRecSel?C_INK:C_DIM, i==capRecSel?C_SELBG:C_BG);
      tft.setCursor(6,y); char fb[20]; fmtHz(r.freqHz,fb);
      tft.printf("%-11s %3d.%ddB %uk", fb, r.powT/10, abs(r.powT%10), r.bwKhz);
    }
    drawFooter("U/D scroll   BACK list");
  } else {
    // The list is 2 capture ACTIONS followed by the saved files, so this screen
    // both starts/stops/saves captures and browses them. capSel indexes the whole
    // combined list (0,1 = actions; 2.. = files).
    int total = CAP_ACTIONS + capN;
    int rows = (SCR_H - 54) / 18;
    int first = (capSel >= rows) ? (capSel - rows + 1) : 0;
    char label[40];
    for (int k=0;k<rows && (first+k)<total;k++){
      int i = first+k;
      int y = 30 + k*18;
      bool sel = (i==capSel);
      bool isAction = (i < CAP_ACTIONS);
      tft.fillRect(0,y,SCR_W,18, sel?C_SELBG:C_BG);
      uint16_t fg = sel ? C_INK : (isAction ? C_ACC : C_DIM);
      tft.setTextColor(fg, sel?C_SELBG:C_BG);
      tft.setCursor(12,y+2);
      if (isAction) {
        if (tlm.capActive) {
          if (i==0) snprintf(label,40,"[STOP + SAVE]  %lu recs", (unsigned long)tlm.capCount);
          else      snprintf(label,40,"[DISCARD capture]");
        } else {
          if (i==0) snprintf(label,40,"+ New EVENT capture");
          else      snprintf(label,40,"+ New IQ snapshot");
        }
        tft.print(label);
      } else {
        int fi = i - CAP_ACTIONS;
        tft.printf("%-14s %lu B", caps[fi].name, (unsigned long)caps[fi].bytes);
      }
    }
    if (capN==0){ tft.setTextColor(C_DIM,C_BG); tft.setCursor(12, 30 + CAP_ACTIONS*18 + 4);
                  tft.print(capListPending?"loading files...":"(no saved captures yet)"); }
    drawFooter(tlm.capActive ? "REC... U/D sel  OK do  BACK menu"
                             : "U/D sel  OK start/view  R del  BACK menu");
  }
}

static void renderSettings(bool full) {
  if (full) { tft.fillScreen(C_BG); drawTopBar(); }
  int y0=40, rh=30;
  char v[24];
  const char* labels[SF_COUNT] = { "Backlight", "Averaging", "Re-init SDR" };
  for (int i=0;i<SF_COUNT;i++){
    int y=y0+i*rh;
    tft.fillRect(0,y,SCR_W,rh, i==setSel?C_SELBG:C_BG);
    tft.setTextFont(4); tft.setTextColor(i==setSel?C_INK:C_DIM, i==setSel?C_SELBG:C_BG);
    tft.setCursor(20,y+3); tft.print(labels[i]);
    if      (i==SF_BL)     snprintf(v,24,"%u%%",(backlight*100)/255);
    else if (i==SF_AVG)    snprintf(v,24,"1/%d", 1<<setAvg);
    else                   snprintf(v,24,"[OK]");
    tft.setCursor(300,y+3); tft.print(v);
  }
  drawFooter(setSel==SF_REINIT ? "OK re-enumerate the dongle  BACK menu"
                               : "U/D field  L/R change  BACK menu");
}

static void render(bool full) {
  switch(scr){
    case SCR_MENU:     renderMenu(full); break;
    case SCR_SPECTRUM: renderSpectrumLike(full, "L/R tune  U/D step  OK cfg  BACK menu"); break;
    case SCR_BANDS:    renderBands(full); break;
    case SCR_SWEEP:    renderSweep(full); break;
    case SCR_DETECT:   renderDetect(full); break;
    case SCR_DEMOD:    renderDemod(full); break;
    case SCR_CONFIG:   renderConfig(full); break;
    case SCR_CAPTURES: renderCaptures(full); break;
    case SCR_SYSTEM:   renderSystem(full); break;
    case SCR_SETTINGS: renderSettings(full); break;
    default: break;
  }
}

// ======================================================================= navigation
static void gotoScreen(Screen s) {
  scr = s; needFull = true;
  // request the matching engine mode
  for (int i=0;i<MENU_N;i++) if (MENU[i].target==s && MENU[i].mode>=0) { cmdU8(CMD_SET_MODE, (uint8_t)MENU[i].mode); break; }
  if (s==SCR_CAPTURES && !inCapView) { capN=0; capSel=0; capListPending=true; sendCmd(CMD_CAP_LIST); }
}

static void changeConfig(int f, int dir) {
  switch(f){
    case CF_FREQ:   cmdI32(CMD_TUNE_STEP, dir*(int32_t)STEPS[stepIdx]); break;
    case CF_SR: {
      static const uint32_t SRS[] = {250000,1024000,1536000,1920000,2048000,2400000,2560000,3200000};
      int n=sizeof(SRS)/sizeof(SRS[0]); int cur=0; uint32_t best=0xFFFFFFFF;
      for(int i=0;i<n;i++){ uint32_t d=SRS[i]>tlm.sampleRateHz?SRS[i]-tlm.sampleRateHz:tlm.sampleRateHz-SRS[i]; if(d<best){best=d;cur=i;} }
      cur += dir; if(cur<0)cur=0; if(cur>=n)cur=n-1; cmdU32(CMD_SET_SR, SRS[cur]); break;
    }
    case CF_GAINMODE: sendSetGain(!tlm.gainAuto, tlm.gainTenthDb); break;
    case CF_GAIN:   { int g=tlm.gainTenthDb + dir*10; if(g<0)g=0; if(g>496)g=496; sendSetGain(false,(int16_t)g); } break;
    case CF_PPM:    cmdI16(CMD_SET_PPM, (int16_t)(tlm.ppm + dir)); break;
    case CF_BIAS:   cmdU8(CMD_SET_BIAST, tlm.biasTee?0:1); break;
    case CF_DIRECT: { int d=tlm.directSamp+dir; if(d<0)d=0; if(d>2)d=2; cmdU8(CMD_SET_DIRECT,(uint8_t)d); } break;
    case CF_FFT:    { cfgFft+=dir; if(cfgFft<SDR_MIN_FFT_LOG2)cfgFft=SDR_MIN_FFT_LOG2; if(cfgFft>SDR_MAX_FFT_LOG2)cfgFft=SDR_MAX_FFT_LOG2; sendSetFft((uint8_t)cfgFft,0,(uint8_t)setAvg); } break;
    case CF_SQUELCH:{ cfgSquelchT += dir*10; if(cfgSquelchT<-800)cfgSquelchT=-800; if(cfgSquelchT>0)cfgSquelchT=0; cmdI16(CMD_SET_SQUELCH,(int16_t)cfgSquelchT); } break;
    case CF_DEMOD:  { int d=tlm.demod+dir; if(d<0)d=0; if(d>5)d=5; cmdU8(CMD_SET_DEMOD,(uint8_t)d); } break;
    case CF_SWSTART:{ int64_t v=(int64_t)cfgSwStart+dir*5000000LL; if(v<1000000)v=1000000; if(v>=(int64_t)cfgSwStop)v=cfgSwStop-5000000; cfgSwStart=(uint32_t)v; sendSetSweep(); } break;
    case CF_SWSTOP: { int64_t v=(int64_t)cfgSwStop +dir*5000000LL; if(v<=(int64_t)cfgSwStart)v=cfgSwStart+5000000; if(v>1766000000LL)v=1766000000LL; cfgSwStop=(uint32_t)v; sendSetSweep(); } break;
    case CF_SWSTEP: { int64_t v=(int64_t)cfgSwStep +dir*500000LL; if(v<100000)v=100000; if(v>10000000)v=10000000; cfgSwStep=(uint32_t)v; sendSetSweep(); } break;
  }
}

static void onButton(uint8_t btn, uint8_t edge) {
  if (edge == EDGE_UP) return;                 // act on press + repeat
  bool rep = (edge == EDGE_REPEAT);

  if (scr == SCR_MENU) {
    if (btn==BTN_UP)   { menuSel=(menuSel-1+MENU_N)%MENU_N; render(false); }
    if (btn==BTN_DOWN) { menuSel=(menuSel+1)%MENU_N; render(false); }
    if (btn==BTN_OK)   { gotoScreen(MENU[menuSel].target); render(true); }
    return;
  }
  if (btn==BTN_BACK && !rep) {
    if (scr==SCR_CAPTURES && inCapView) { inCapView=false; needFull=true; render(true); return; }
    scr=SCR_MENU; needFull=true; render(true); return;
  }

  switch(scr){
    case SCR_SPECTRUM: case SCR_DETECT: case SCR_SWEEP:
      if (btn==BTN_LEFT)  cmdI32(CMD_TUNE_STEP, -(int32_t)STEPS[stepIdx]);
      if (btn==BTN_RIGHT) cmdI32(CMD_TUNE_STEP,  (int32_t)STEPS[stepIdx]);
      if (btn==BTN_UP && !rep)   { stepIdx=(stepIdx+1)%STEP_N; snprintf(toast,40,"step %lu Hz",(unsigned long)STEPS[stepIdx]); toastMs=millis(); }
      if (btn==BTN_DOWN && !rep) { stepIdx=(stepIdx-1+STEP_N)%STEP_N; snprintf(toast,40,"step %lu Hz",(unsigned long)STEPS[stepIdx]); toastMs=millis(); }
      if (btn==BTN_OK && !rep)   { gotoScreen(SCR_CONFIG); render(true); }
      break;
    case SCR_DEMOD:
      if (btn==BTN_LEFT)  cmdI32(CMD_TUNE_STEP, -(int32_t)STEPS[stepIdx]);
      if (btn==BTN_RIGHT) cmdI32(CMD_TUNE_STEP,  (int32_t)STEPS[stepIdx]);
      if (btn==BTN_UP && !rep)   { int d=(tlm.demod+1); if(d>5)d=1; cmdU8(CMD_SET_DEMOD,(uint8_t)d); }
      if (btn==BTN_DOWN && !rep) { int d=(tlm.demod-1); if(d<1)d=5; cmdU8(CMD_SET_DEMOD,(uint8_t)d); }
      break;
    case SCR_BANDS:
      if (btn==BTN_UP)   { bandSel=(bandSel-1+BAND_N)%BAND_N; render(false); }
      if (btn==BTN_DOWN) { bandSel=(bandSel+1)%BAND_N; render(false); }
      if (btn==BTN_OK && !rep) { tuneToBand(bandSel); cmdU8(CMD_SET_MODE, LMODE_SPECTRUM); gotoScreen(SCR_SPECTRUM); render(true); }
      break;
    case SCR_CONFIG:
      if (btn==BTN_UP)   { cfgSel=(cfgSel-1+CF_COUNT)%CF_COUNT; render(false); }
      if (btn==BTN_DOWN) { cfgSel=(cfgSel+1)%CF_COUNT; render(false); }
      if (btn==BTN_LEFT)  changeConfig(cfgSel,-1);
      if (btn==BTN_RIGHT) changeConfig(cfgSel, 1);
      if (btn==BTN_OK && !rep) changeConfig(cfgSel, 1);
      break;
    case SCR_CAPTURES:
      if (inCapView) {
        if (btn==BTN_UP)   { if(capRecSel>0)capRecSel--; render(false); }
        if (btn==BTN_DOWN) { if(capRecSel<capRecN-1)capRecSel++; render(false); }
      } else {
        int total = CAP_ACTIONS + capN;
        if (btn==BTN_UP)   { if(capSel>0)capSel--; render(false); }
        if (btn==BTN_DOWN) { if(capSel<total-1)capSel++; render(false); }
        if (btn==BTN_OK && !rep) {
          if (capSel < CAP_ACTIONS) {                 // capture-control actions
            if (tlm.capActive) {
              if (capSel==0) { sendCmd(CMD_CAP_SAVE); snprintf(toast,40,"saving..."); }
              else           { sendCmd(CMD_CAP_DISCARD); snprintf(toast,40,"discarded"); }
            } else {
              uint8_t kind = (capSel==0) ? 0 : 1;      // 0 events, 1 IQ snapshot
              cmdU8(CMD_CAP_START, kind);
              snprintf(toast,40, kind? "IQ capture started":"event capture started");
            }
            toastMs=millis(); sendReq(REQ_TELEMETRY); render(false);
          } else if (capN>0) {                         // view a saved file
            int fi = capSel - CAP_ACTIONS;
            uint8_t a[16]; memcpy(a,caps[fi].name,16);
            capRecN=0; capRecSel=0; inCapView=true; sendCmd(CMD_CAP_VIEW,a,16); needFull=true; render(true);
          }
        }
        if (btn==BTN_RIGHT && !rep && capSel>=CAP_ACTIONS && capN>0) {
          int fi = capSel - CAP_ACTIONS;
          uint8_t a[16]; memcpy(a,caps[fi].name,16); sendCmd(CMD_CAP_DELETE,a,16);
          snprintf(toast,40,"deleted"); toastMs=millis();
          capN=0; capSel=CAP_ACTIONS; capListPending=true; sendCmd(CMD_CAP_LIST);
        }
      }
      break;
    case SCR_SETTINGS:
      if (btn==BTN_UP)   { setSel=(setSel-1+SF_COUNT)%SF_COUNT; render(false); }
      if (btn==BTN_DOWN) { setSel=(setSel+1)%SF_COUNT; render(false); }
      if (btn==BTN_LEFT || btn==BTN_RIGHT) {
        int dir = (btn==BTN_RIGHT)?1:-1;
        if (setSel==SF_BL) { int b=backlight + dir*16; if(b<16)b=16; if(b>255)b=255; backlight=b; ledcWrite(PIN_LCD_BL, backlight); }
        else if (setSel==SF_AVG) { setAvg += dir; if(setAvg<0)setAvg=0; if(setAvg>6)setAvg=6; sendSetFft((uint8_t)cfgFft,0,(uint8_t)setAvg); }
        render(false);
      }
      if (btn==BTN_OK && !rep && setSel==SF_REINIT) { sendCmd(CMD_REBOOT_SDR); snprintf(toast,40,"re-initialising SDR..."); toastMs=millis(); render(false); }
      break;
    default: break;
  }
}

// ======================================================================= link RX
static void onFrame(uint8_t msg, const uint8_t* p, uint16_t len) {
  lastRxMs = millis();
  switch(msg){
    case MSG_HELLO_TEENSY:
      if (len>=3){ fwMajor=p[1]; fwMinor=p[2]; }
      break;
    case MSG_TELEMETRY:
      if (len>=TLM_LEN){
        tlm.tunedHz=lp_get32(&p[0]); tlm.sampleRateHz=lp_get32(&p[4]);
        tlm.mode=p[8]; tlm.gainAuto=p[9];
        tlm.gainTenthDb=lp_geti16(&p[10]); tlm.ppm=lp_geti16(&p[12]);
        tlm.biasTee=p[14]; tlm.directSamp=p[15]; tlm.demod=p[16]; tlm.tunerType=p[17];
        tlm.sdrPresent=p[18]; tlm.sdrStreaming=p[19];
        tlm.rssiTenthDbfs=lp_geti16(&p[20]); tlm.floorTenthDbfs=lp_geti16(&p[22]);
        tlm.busMv=lp_get16(&p[24]); tlm.curMa=lp_geti16(&p[26]); tlm.powCw=lp_get16(&p[28]);
        tlm.dieTenthC=lp_geti16(&p[30]); tlm.fanDuty=p[32]; tlm.sdOk=p[33];
        tlm.capActive=p[34]; tlm.capKind=p[35];
        tlm.capCount=lp_get32(&p[36]); tlm.capBytes=lp_get32(&p[40]);
        tlm.sdFreeMB=lp_get32(&p[44]); tlm.sdTotalMB=lp_get32(&p[48]);
        tlm.gpsValid=p[52]; tlm.gpsSats=p[53];
        tlm.latE7=lp_geti32(&p[54]); tlm.lonE7=lp_geti32(&p[58]);
        tlm.altM=lp_geti16(&p[62]); tlm.hh=p[64]; tlm.mm=p[65]; tlm.ss=p[66];
        tlm.valid=true;
      }
      break;
    case MSG_SPECTRUM:
      if (len>=SPEC_HDR){
        specCentre=lp_get32(&p[0]); specSpan=lp_get32(&p[4]);
        uint16_t nb=lp_get16(&p[8]); if(nb>512)nb=512;
        specRefT=lp_geti16(&p[10]); specFloorT=lp_geti16(&p[12]);
        uint16_t avail = len-SPEC_HDR; if(avail<nb)nb=avail;
        memcpy(specMag,&p[SPEC_HDR],nb);
        specBins=nb;                 // actual bins copied (never > bytes present)
        specNew=true;
      }
      break;
    case MSG_DETECT:
      if (len>=DET_LEN){
        Det d; d.freqHz=lp_get32(&p[0]); d.powT=lp_geti16(&p[4]); d.bwKhz=lp_get16(&p[6]);
        d.tMs=lp_get32(&p[8]); d.kind=p[12];
        dets[detHead]=d; detHead=(detHead+1)%DET_MAX; if(detN<DET_MAX)detN++;
        if (scr==SCR_DETECT || scr==SCR_SWEEP) renderDetList(WF_Y, WF_H);  // show it now
      }
      break;
    case MSG_ACK: break;
    case MSG_LOG:
      if (len>0){ strncpy(toast,(const char*)p,39); toast[39]=0; toastMs=millis(); }
      break;
    case MSG_CAP_ITEM:
      if (len>=22){
        uint8_t idx=p[0], total=p[1];
        if (idx==0) capN=0;
        if (total>0 && capN<CAP_MAX){ memcpy(caps[capN].name,&p[2],16); caps[capN].name[15]=0; caps[capN].bytes=lp_get32(&p[18]); capN++; }
        capListPending=false;
        if (scr==SCR_CAPTURES) render(false);
      }
      break;
    case MSG_CAP_REC:
      if (len>=16 && capRecN<CAPREC_MAX){
        CapRec r; r.freqHz=lp_get32(&p[2]); r.powT=lp_geti16(&p[6]); r.bwKhz=lp_get16(&p[8]);
        r.tMs=lp_get32(&p[10]); r.kind=p[14];
        capRecs[capRecN++]=r;
        if (scr==SCR_CAPTURES && inCapView) render(false);
      }
      break;
    case MSG_PING: { uint8_t e[4]={0,0,0,0}; for(int i=0;i<4&&i<len;i++)e[i]=p[i]; linkSend(MSG_PONG,e,4);} break;
    default: break;
  }
}

static void linkRx() {
  uint8_t oseq,oflags,ochan,omsg; const uint8_t* opay; uint16_t oplen;
  int budget=4096;
  while (LinkSerial.available() && budget-->0){
    uint8_t c=(uint8_t)LinkSerial.read();
    if (parser.feed(c,oseq,oflags,ochan,omsg,opay,oplen)){
      // forwarded button events drive the local UI
      if (omsg==MSG_INPUT && oplen>=2) onButton(opay[0],opay[1]);
      else onFrame(omsg,opay,oplen);
    }
  }
}

// ======================================================================= boot splash
static uint16_t heat565(uint8_t v){ uint8_t r,g,b; heatRGB(v,r,g,b); return tft.color565(r,g,b); }

static void bootSplash() {
  tft.fillScreen(C_BG);
  tft.setTextFont(4); tft.setTextColor(C_ACC, C_BG);
  tft.setCursor(96, 34); tft.print("RTL-SDR");
  tft.setCursor(120, 72); tft.print("PENTEST");
  tft.setTextFont(2); tft.setTextColor(C_DIM, C_BG);
  tft.setCursor(112, 112); tft.print("handheld spectrum scanner");
  tft.setCursor(214, 296); tft.printf("v%d.%d", FW_MAJOR, FW_MINOR);

  const int baseY = 250, topY = 150;
  uint32_t lcg = 0xC0FFEE;
  for (int f = 0; f <= SCR_W; f += 10) {
    for (int x = (f-10 < 0 ? 0 : f-10); x < f && x < SCR_W; x++) {
      // synthetic spectrum: noise floor + two peaks, revealed left-to-right
      float m = 6.0f
              + 70.0f * expf(-((x-150.0f)*(x-150.0f)) / 900.0f)
              + 46.0f * expf(-((x-330.0f)*(x-330.0f)) / 380.0f);
      lcg = lcg*1664525u + 1013904223u;
      m += (float)((lcg >> 27) & 0x0F);
      int h = (int)m; if (h > (baseY-topY)) h = baseY-topY;
      int y = baseY - h;
      uint8_t idx = (uint8_t)(h * 255 / (baseY-topY));
      tft.drawFastVLine(x, y, baseY - y, heat565(idx));
    }
    tft.fillRect(20, 284, f * (SCR_W-40) / SCR_W, 5, C_OK);   // progress bar
    delay(16);
  }
  tft.setTextColor(C_OK, C_BG); tft.setCursor(20, 300); tft.print("ready");
  delay(350);
}

// ======================================================================= setup / loop
void setup() {
  Serial.begin(115200);   // CH340 console on UART0, independent of the link
  pinMode(PIN_LED_R,OUTPUT); digitalWrite(PIN_LED_R,HIGH);
  pinMode(PIN_LED_G,OUTPUT); digitalWrite(PIN_LED_G,HIGH);
  pinMode(PIN_LED_B,OUTPUT); digitalWrite(PIN_LED_B,HIGH);
  pinMode(PIN_AUDIO_EN,OUTPUT); digitalWrite(PIN_AUDIO_EN,HIGH);  // amp off

  ledcAttach(PIN_LCD_BL, 5000, 8);
  ledcWrite(PIN_LCD_BL, 0);

  tft.init();
  tft.setRotation(1);       // 480x320 landscape
  tft.fillScreen(C_BG);
  buildPalette();
  wfInit();
  ledcWrite(PIN_LCD_BL, backlight);

  bootSplash();                 // animated intro before the UI comes up

  memset(&tlm,0,sizeof(tlm));

  // Enlarge the UART2 RX ring: the default (256 B) can overflow while a waterfall
  // frame is rendering, dropping bytes and failing CRC. A spectrum frame is ~528 B.
  LinkSerial.setRxBufferSize(4096);
  LinkSerial.begin(LINK_BAUD, SERIAL_8N1, LINK_RX, LINK_TX);
  parser.reset();

  render(true);
  sendHelloDisp();
  sendReq(REQ_HELLO);
}

void loop() {
  linkRx();

  // push a waterfall row + refresh trace when new spectrum arrived and we're on a spectrum-ish screen
  if (specNew) {
    specNew=false;
    if (scr==SCR_SPECTRUM){ wfPush(); drawTrace(); }
    else if (scr==SCR_DETECT || scr==SCR_SWEEP){ drawTrace(); }
    else if (scr==SCR_DEMOD){ /* meter refresh handled by telemetry tick */ }
  }

  // periodic light refresh of HUD/telemetry-driven screens
  static uint32_t lastUi=0;
  uint32_t now=millis();
  if (now-lastUi>=200){
    lastUi=now;
    if (scr==SCR_SPECTRUM){ drawTopBar(); drawHud(); }
    else if (scr==SCR_DETECT||scr==SCR_SWEEP){ drawTopBar(); drawHud(); renderDetList(WF_Y, WF_H); }
    else if (scr==SCR_DEMOD){ renderDemod(false); }
    else if (scr==SCR_SYSTEM){ renderSystem(false); }
    else if (scr==SCR_MENU){ drawTopBar(); }
    if (toast[0] && (now-toastMs<2600)) { /* footer shows toast via render paths */ }
  }

  // heartbeat if the link looks down
  if (now-lastHelloMs>=LINK_HELLO_MS){
    lastHelloMs=now;
    if (lastRxMs==0 || (now-lastRxMs)>=LINK_TIMEOUT_MS){ sendHelloDisp(); sendReq(REQ_HELLO); }
  }
}
