// Shared UI theme, app framework and globals.
#pragma once

#include <Arduino.h>
#include <lvgl.h>

// ---------------------------------------------------------------- theme
// True black background: on an AMOLED an unlit pixel draws no current,
// so black is both the iOS dark look and the cheapest thing to display.
#define COL_BG        lv_color_hex(0x000000)
#define COL_CARD      lv_color_hex(0x1C1C1E)
#define COL_CARD_HI   lv_color_hex(0x2C2C2E)
#define COL_SEP       lv_color_hex(0x38383A)
#define COL_TEXT      lv_color_hex(0xFFFFFF)
#define COL_TEXT_2    lv_color_hex(0x8E8E93)
#define COL_BLUE      lv_color_hex(0x0A84FF)
#define COL_INDIGO    lv_color_hex(0x5E5CE6)
#define COL_GREEN     lv_color_hex(0x30D158)
#define COL_ORANGE    lv_color_hex(0xFF9F0A)
#define COL_RED       lv_color_hex(0xFF453A)
#define COL_YELLOW    lv_color_hex(0xFFD60A)
#define COL_VIOLET    lv_color_hex(0xBF5AF2)
#define COL_TEAL      lv_color_hex(0x40C8E0)

#define STATUSBAR_H   34
#define SCREEN_W      410
#define SCREEN_H      502

// The panel's corners are rounded, so anything parked in a corner gets clipped.
// Keep content inside this box. Bump SAFE_X if the battery text still clips.
#define SAFE_X        34
#define SAFE_TOP      30
#define SAFE_BOT      28

// ---------------------------------------------------------------- app framework
typedef struct {
  const char *name;
  const char *icon;        // LVGL symbol
  lv_color_t (*tint)(void);
  void (*open)(lv_obj_t *body);   // build UI into `body` (below the header)
  void (*close)(void);            // stop radios / free resources
  void (*tick)(void);             // called ~5 Hz while open
} app_t;

extern const app_t APPS[];
extern const int   APP_COUNT;

void ui_init(void);
void ui_open_app(int idx);
void ui_open_app_by_name(const char *name);
void ui_go_home(void);
void ui_tick(void);
int  ui_active_app(void);

// Shared widget helpers
lv_obj_t *ui_card(lv_obj_t *parent);
lv_obj_t *ui_row(lv_obj_t *parent, const char *title, const char *sub);
void      ui_status_text(lv_obj_t *label, const char *fmt, ...);

// App builders (open/close/tick triple each)
void app_tail_open(lv_obj_t*);      void app_tail_close(void);      void app_tail_tick(void);
void app_probe_open(lv_obj_t*);     void app_probe_close(void);     void app_probe_tick(void);
void app_deauthd_open(lv_obj_t*);   void app_deauthd_close(void);   void app_deauthd_tick(void);
void app_handshake_open(lv_obj_t*); void app_handshake_close(void); void app_handshake_tick(void);
void app_ultra_open(lv_obj_t*);     void app_ultra_close(void);     void app_ultra_tick(void);
void app_bugsweep_open(lv_obj_t*);  void app_bugsweep_close(void);  void app_bugsweep_tick(void);
void app_evidence_open(lv_obj_t*);  void app_evidence_close(void);  void app_evidence_tick(void);
void app_sensors_open(lv_obj_t*);   void app_sensors_close(void);   void app_sensors_tick(void);
void app_watchface_open(lv_obj_t*);   void app_watchface_close(void);   void app_watchface_tick(void);
void app_settings_open(lv_obj_t*);    void app_settings_close(void);    void app_settings_tick(void);
void app_torch_open(lv_obj_t*);       void app_torch_close(void);       void app_torch_tick(void);
void app_stopwatch_open(lv_obj_t*);   void app_stopwatch_close(void);   void app_stopwatch_tick(void);
void app_heatmap_open(lv_obj_t*);     void app_heatmap_close(void);     void app_heatmap_tick(void);
void app_exposure_open(lv_obj_t*);    void app_exposure_close(void);    void app_exposure_tick(void);
void app_sdr_open(lv_obj_t*);         void app_sdr_close(void);         void app_sdr_tick(void);
void app_link_open(lv_obj_t*);        void app_link_close(void);        void app_link_tick(void);
void app_touchcal_open(lv_obj_t*);    void app_touchcal_close(void);    void app_touchcal_tick(void);
bool touch_raw(int32_t *x, int32_t *y);   // raw FT3168 read, for touch calibration

// Tail can be launched targeting a specific device the Profiler picked.
void tail_set_target(const uint8_t *mac, const char *name, bool isBle);

// Shared: a picked target passed from Profiler to Tail.
extern uint8_t  g_target_mac[6];
extern char     g_target_name[28];
extern bool     g_target_ble;
extern bool     g_target_set;

// ---------------------------------------------------------------- globals (defined in rtlsdr-watch.ino)
struct SysState {
  float    battVolt;     // mV
  int      battPct;
  bool     charging;
  bool     vbus;
  uint16_t vbusVolt;     // mV
  uint16_t sysVolt;      // mV
  float    tImu;         // QMI8658 die temp, C
  float    tPmu;         // AXP2101 die temp, C
  float    tEsp;         // ESP32-S3 internal temp, C
  uint8_t  hh, mm, ss;
  uint16_t year; uint8_t month, day;
  bool     rtcOk;
  bool     imuOk;
  bool     pmuOk;
};

// 24h -> 12h. Returns the hour; *ampm gets "AM"/"PM".
uint8_t hour12(uint8_t h24, const char **ampm);
extern SysState SYS;

void sys_refresh(void);          // re-read PMU + RTC
bool sys_imu_read(float *ax, float *ay, float *az, float *gx, float *gy, float *gz);
void buzz(uint16_t ms);          // haptic tap on GPIO18 (honors g_haptics)

// Panel + power controls (defined in the .ino), driven by Settings and Torch.
void    ui_set_brightness(uint8_t b);
uint8_t ui_get_brightness(void);
void    ui_set_charge_ma(int ma);
extern bool g_haptics;           // Settings toggles the motor

// ---- watch-wide modes (stackable), defined in the .ino ----
extern bool g_dark;              // screen off + touch off; scanners keep running
extern bool g_stealth;           // Stealth Log armed (dark + keep logging)
extern bool g_mode_protection;   // harden: block ALL transmit
extern bool g_mode_cloak;        // full passive: dark + RX-only + no transmit
void ui_enter_dark(void);        // go dark now (screen off, inputs off, keep scanning)
void ui_wake(void);              // BOOT: restore brightness + touch, clear dark modes
bool ui_is_dark(void);

// ---- charge screen: booted from USB while off -> show charge status, PWR enters UI ----
extern bool g_charge_mode;
void ui_show_charge(void);       // build + show the charge screen (instead of booting UI)
void ui_charge_tick(void);       // update battery % / state on the charge screen
void ui_charge_enter_ui(void);   // PWR pressed on charge screen -> boot into the UI
// A picked device the wardrive engine correlated into a profile (Profiles viewer).
// (raw sightings -> fingerprints.csv, correlated profiles -> profiles.csv)

// Copy only printable ASCII into dst. Raw SSIDs / BLE names carry control bytes,
// emoji and invalid UTF-8 that make LVGL's text renderer crash -- always run
// anything from the air through this before putting it in a label.
static inline void ui_safe(char *dst, size_t n, const char *src) {
  size_t j = 0;
  if (src) for (size_t i = 0; src[i] && j < n - 1; i++) {
    unsigned char c = (unsigned char)src[i];
    if (c >= 32 && c <= 126) dst[j++] = c;
  }
  dst[j] = 0;
  if (j == 0) { snprintf(dst, n, "(unnamed)"); }
}

lv_obj_t *ui_home_screen(void);   // for the boot animation to hand off to
void      boot_play(void);        // ctOS eye draw-on, then loads home

// --- serial + input state (defined in rtlsdr-watch.ino) ---
#include "HWCDC.h"
extern HWCDC USBSerial;
extern volatile bool g_ui_touching;   // true while a finger is down (scroll-safe rebuilds)
