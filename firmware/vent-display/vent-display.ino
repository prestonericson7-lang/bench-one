// ============================================================
//  vent-display — Teensy 4.1 dashboard in the F30 driver vent
//
//  2.4" ILI9341 (SPI) + EC11 encoder, mounted in the printed vent insert
//  (../../hardware/3d/vent-insert.scad).
//
//  WHY THE TEENSY IS HERE AND NOT IN THE REMOTE BOX
//    The panel is SPI at tens of MHz. Running that down a multi-foot loom to a
//    glovebox enclosure produces reflections and corruption, and long encoder
//    runs pick up noise as phantom counts. So the MCU sits AT the vent and the
//    only thing crossing the car is one robust cable: 12 V + an async link.
//    See ../../hardware/interconnect.md.
//
//  TELEMETRY IN
//    Line-oriented ASCII on Serial1 from the main box, so it is debuggable with
//    a terminal and survives a noisy line (a corrupt line is dropped, not
//    misparsed). One key=value pair per line:
//        SOC=64.5
//        RANGE=31.2
//        SPEED=45
//        AIRT=18.4
//        CASET=41.0
//        VOLT=14.1
//        DAMPER=OPEN
//        STAT=recording
//
//  DISPLAY DISCIPLINE
//    Full-screen repaints flicker and are slow. Each field redraws ONLY when its
//    value actually changes, over its own background rectangle. Page changes are
//    the only full repaint.
// ============================================================
#include <ILI9341_t3.h>
#include <Encoder.h>

// ---------------- pins ----------------
#define TFT_CS    10
#define TFT_DC     9
#define TFT_RST    8
#define ENC_A      2
#define ENC_B      3
#define ENC_SW     4
#define LINK       Serial1      // to the main enclosure
#define LINK_BAUD  115200

// ---------------- colours (dark, legible at night) ----------------
#define C_BG      ILI9341_BLACK
#define C_LABEL   0x7BEF        // grey
#define C_VALUE   ILI9341_WHITE
#define C_GOOD    0x07E0
#define C_WARN    0xFD20
#define C_BAD     0xF800
#define C_ACCENT  0x05FF

ILI9341_t3 tft = ILI9341_t3(TFT_CS, TFT_DC, TFT_RST);
Encoder enc(ENC_A, ENC_B);

// ---------------- telemetry ----------------
struct Telem {
  float soc = NAN, range = NAN, speed = NAN;
  float airT = NAN, caseT = NAN, volt = NAN;
  char  damper[8] = "?";
  char  stat[16]  = "-";
  uint32_t lastRx = 0;
};
static Telem T, Tdrawn;          // Tdrawn = what is currently on screen

enum Page { PAGE_DRIVE = 0, PAGE_CLIMATE, PAGE_SYSTEM, PAGE_COUNT };
static int  g_page = PAGE_DRIVE, g_pageDrawn = -1;
static long g_encLast = 0;

// ============================================================
//  link parsing -- tolerant: a mangled line is dropped, never half-applied
// ============================================================
static void applyKV(const char *k, const char *v) {
  if      (!strcmp(k, "SOC"))    T.soc   = atof(v);
  else if (!strcmp(k, "RANGE"))  T.range = atof(v);
  else if (!strcmp(k, "SPEED"))  T.speed = atof(v);
  else if (!strcmp(k, "AIRT"))   T.airT  = atof(v);
  else if (!strcmp(k, "CASET"))  T.caseT = atof(v);
  else if (!strcmp(k, "VOLT"))   T.volt  = atof(v);
  else if (!strcmp(k, "DAMPER")) { strncpy(T.damper, v, 7); T.damper[7] = 0; }
  else if (!strcmp(k, "STAT"))   { strncpy(T.stat, v, 15);  T.stat[15]  = 0; }
  else return;
  T.lastRx = millis();
}

static void pollLink() {
  static char buf[64];
  static uint8_t idx = 0;
  while (LINK.available()) {
    char c = LINK.read();
    if (c == '\n' || c == '\r') {
      buf[idx] = 0;
      char *eq = strchr(buf, '=');
      if (eq && idx > 2) { *eq = 0; applyKV(buf, eq + 1); }
      idx = 0;
    } else if (idx < sizeof(buf) - 1) {
      buf[idx++] = c;
    } else {
      idx = 0;                   // overlong line: drop it rather than truncate
    }
  }
}

// ============================================================
//  drawing helpers -- only repaint what changed
// ============================================================
static void label(int x, int y, const char *s) {
  tft.setTextColor(C_LABEL, C_BG); tft.setTextSize(1);
  tft.setCursor(x, y); tft.print(s);
}

static void bigValue(int x, int y, const char *txt, uint16_t col, int size) {
  tft.setTextSize(size);
  tft.setTextColor(col, C_BG);
  tft.setCursor(x, y);
  tft.print(txt);
}

static void fmt(char *out, size_t n, float v, int dp, const char *unit) {
  if (isnan(v)) snprintf(out, n, "--%s", unit);
  else          snprintf(out, n, "%.*f%s", dp, v, unit);
}

// clear a field area then write, so shorter values don't leave ghosts
static void field(int x, int y, int w, int h, const char *txt,
                  uint16_t col, int size) {
  tft.fillRect(x, y, w, h, C_BG);
  bigValue(x, y, txt, col, size);
}

// ============================================================
//  pages
// ============================================================
static void drawHeader(const char *title) {
  tft.fillRect(0, 0, tft.width(), 18, C_BG);
  tft.setTextSize(1);
  tft.setTextColor(C_ACCENT, C_BG);
  tft.setCursor(4, 5); tft.print(title);
  // stale-link marker: silence matters more than any single value
  bool stale = (millis() - T.lastRx) > 3000;
  tft.setTextColor(stale ? C_BAD : C_GOOD, C_BG);
  tft.setCursor(tft.width() - 34, 5);
  tft.print(stale ? "LINK" : "  ok");
}

static void pageDrive(bool full) {
  char s[16];
  if (full) {
    tft.fillScreen(C_BG);
    label(6, 26, "BATTERY");
    label(6, 96, "EV RANGE");
    label(140, 96, "SPEED");
  }
  if (full || T.soc != Tdrawn.soc) {
    fmt(s, sizeof s, T.soc, 1, "%");
    uint16_t c = isnan(T.soc) ? C_LABEL : (T.soc < 15 ? C_BAD :
                 (T.soc < 30 ? C_WARN : C_GOOD));
    field(6, 40, 220, 44, s, c, 5);
    // bar underneath gives a glanceable read without reading digits
    int w = isnan(T.soc) ? 0 : (int)(T.soc / 100.0f * (tft.width() - 12));
    tft.fillRect(6, 86, tft.width() - 12, 5, 0x2104);
    if (w > 0) tft.fillRect(6, 86, w, 5, c);
  }
  if (full || T.range != Tdrawn.range) {
    fmt(s, sizeof s, T.range, 1, "mi");
    field(6, 108, 120, 26, s, C_VALUE, 3);
  }
  if (full || T.speed != Tdrawn.speed) {
    fmt(s, sizeof s, T.speed, 0, "");
    field(140, 108, 100, 26, s, C_VALUE, 3);
  }
}

static void pageClimate(bool full) {
  char s[16];
  if (full) {
    tft.fillScreen(C_BG);
    label(6, 26, "VENT AIR");
    label(6, 78, "ENCLOSURE");
    label(6, 130, "DAMPER");
  }
  if (full || T.airT != Tdrawn.airT) {
    fmt(s, sizeof s, T.airT, 1, "C");
    uint16_t c = isnan(T.airT) ? C_LABEL : (T.airT > 30 ? C_BAD :
                 (T.airT < 18 ? C_ACCENT : C_VALUE));
    field(6, 38, 200, 32, s, c, 4);
  }
  if (full || T.caseT != Tdrawn.caseT) {
    fmt(s, sizeof s, T.caseT, 1, "C");
    uint16_t c = isnan(T.caseT) ? C_LABEL : (T.caseT > 60 ? C_BAD :
                 (T.caseT > 45 ? C_WARN : C_GOOD));
    field(6, 90, 200, 32, s, c, 4);
  }
  if (full || strcmp(T.damper, Tdrawn.damper)) {
    bool open = !strcmp(T.damper, "OPEN");
    field(6, 142, 200, 26, T.damper, open ? C_ACCENT : C_LABEL, 3);
  }
}

static void pageSystem(bool full) {
  char s[16];
  if (full) {
    tft.fillScreen(C_BG);
    label(6, 26, "12V RAIL");
    label(6, 78, "STATUS");
    label(6, 130, "LINK AGE");
  }
  if (full || T.volt != Tdrawn.volt) {
    fmt(s, sizeof s, T.volt, 1, "V");
    // 12.0 is the logger's cutoff; warn before we get there
    uint16_t c = isnan(T.volt) ? C_LABEL : (T.volt < 12.0f ? C_BAD :
                 (T.volt < 12.4f ? C_WARN : C_GOOD));
    field(6, 38, 200, 32, s, c, 4);
  }
  if (full || strcmp(T.stat, Tdrawn.stat))
    field(6, 90, 230, 26, T.stat, C_VALUE, 2);

  static uint32_t lastAge = 0;
  uint32_t age = (millis() - T.lastRx) / 1000;
  if (full || age != lastAge) {
    lastAge = age;
    snprintf(s, sizeof s, "%lus", (unsigned long)age);
    field(6, 142, 150, 26, s, age > 3 ? C_BAD : C_LABEL, 3);
  }
}

// ============================================================
void setup() {
  Serial.begin(115200);
  LINK.begin(LINK_BAUD);
  pinMode(ENC_SW, INPUT_PULLUP);

  tft.begin();
  tft.setRotation(3);                 // landscape: the vent aperture is wide
  tft.fillScreen(C_BG);
  tft.setTextColor(C_ACCENT);
  tft.setTextSize(2);
  tft.setCursor(10, 60);
  tft.print("vent-display");
  tft.setTextSize(1);
  tft.setCursor(10, 90);
  tft.print("waiting for telemetry...");
  g_encLast = enc.read() / 4;
}

void loop() {
  pollLink();

  // encoder turns pages; push also advances, so one control does everything
  long e = enc.read() / 4;
  if (e != g_encLast) {
    g_page = (int)((g_page + (e - g_encLast)) % PAGE_COUNT);
    if (g_page < 0) g_page += PAGE_COUNT;
    g_encLast = e;
  }
  static uint32_t lastBtn = 0;
  if (!digitalRead(ENC_SW) && millis() - lastBtn > 250) {
    lastBtn = millis();
    g_page = (g_page + 1) % PAGE_COUNT;
  }

  static uint32_t lastDraw = 0;
  if (millis() - lastDraw > 100) {     // 10 Hz is plenty and keeps SPI quiet
    lastDraw = millis();
    bool full = (g_page != g_pageDrawn);
    if (full) g_pageDrawn = g_page;

    drawHeader(g_page == PAGE_DRIVE ? "DRIVE" :
               g_page == PAGE_CLIMATE ? "CLIMATE" : "SYSTEM");
    switch (g_page) {
      case PAGE_DRIVE:   pageDrive(full);   break;
      case PAGE_CLIMATE: pageClimate(full); break;
      default:           pageSystem(full);  break;
    }
    Tdrawn = T;
  }
}
