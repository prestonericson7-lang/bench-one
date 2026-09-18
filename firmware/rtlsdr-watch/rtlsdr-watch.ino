/*
  WEDJAT — an iOS-style launcher for the Waveshare ESP32-S3-Touch-AMOLED-2.06
  with WiFi scanner, BLE scanner and a sensors/power app.

  Board settings (Tools menu) — all of these matter:
    Board            : ESP32S3 Dev Module
    USB CDC On Boot  : Enabled
    USB Mode         : Hardware CDC and JTAG
    PSRAM            : OPI PSRAM          <-- not QSPI, LVGL needs this
    Flash Size       : 16MB               <-- yes, 16 on a 32MB part
    Partition Scheme : 16M Flash (3MB APP/9.9MB FATFS)

  Libraries: use the bundled set from the Waveshare repo
  (examples/arduino/libraries). The Arduino_GFX in there is a fork whose
  Arduino_CO5300 takes 9 constructor args; stock v1.6.0 takes 10 and will
  compile fine while producing a 502x22 display and a black screen.
*/

#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>

#include "pin_config.h"
#include "ui.h"
#include "touch_cal.h"

#include "Arduino_GFX_Library.h"
#include "Arduino_DriveBus_Library.h"
#include "XPowersLib.h"
#include "SensorQMI8658.hpp"
#include "SensorPCF85063.hpp"

#include "HWCDC.h"
HWCDC USBSerial;

// The Arduino loopTask (Core 1) runs lv_task_handler(), so ALL LVGL rendering
// happens on its stack. The 8 KB default overflows when the SDR app renders its
// data-filled spectrum chart + readouts (a deep LVGL draw), which only happens
// once ESP-NOW frames arrive -> Core 1 LoadProhibited. 16 KB gives the draw room.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

// ---------------------------------------------------------------- hardware
Arduino_DataBus *bus = new Arduino_ESP32QSPI(
  LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);

// 9 args — matches the bundled fork. col_offset1 = 22 is mandatory.
Arduino_CO5300 *gfx = new Arduino_CO5300(
  bus, LCD_RESET, 0 /*rotation*/, LCD_WIDTH, LCD_HEIGHT,
  LCD_COL_OFFSET, 0, 0, 0);

std::shared_ptr<Arduino_IIC_DriveBus> IIC_Bus =
  std::make_shared<Arduino_HWIIC>(IIC_SDA, IIC_SCL, &Wire);

void touch_isr(void);
std::unique_ptr<Arduino_IIC> FT3168(new Arduino_FT3x68(
  IIC_Bus, FT3168_DEVICE_ADDRESS, TP_RESET, TP_INT, touch_isr));
void touch_isr(void) { FT3168->IIC_Interrupt_Flag = true; }

XPowersPMU       PMU;
SensorQMI8658    IMU;
SensorPCF85063   RTC;

SysState SYS = {0};
volatile bool g_ui_touching = false;   // finger-down flag for scroll-safe list rebuilds
bool     g_haptics    = true;          // Settings toggles this; buzz() honors it
uint8_t  g_brightness = 0xD0;          // Settings/Torch drive this via ui_set_brightness()

// ---- watch-wide stackable modes ----
bool     g_dark           = false;     // screen off + touch off; scanners keep running
bool     g_stealth        = false;     // Stealth Log armed
bool     g_mode_protection= false;     // harden: block all TX (radio_tx honors it)
bool     g_mode_cloak     = false;     // full passive: dark + RX-only
bool     g_charge_mode    = false;     // booted from USB while off -> charge screen, not UI
static uint8_t g_wake_bri = 0xD0;      // brightness to restore when BOOT wakes us

// ---------------------------------------------------------------- LVGL plumbing
static lv_display_t *disp;
static lv_color_t   *draw_buf;

static uint32_t millis_cb(void) { return millis(); }

static void disp_flush(lv_display_t *d, const lv_area_t *area, uint8_t *px_map) {
  uint32_t w = lv_area_get_width(area);
  uint32_t h = lv_area_get_height(area);
  gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)px_map, w, h);
  lv_display_flush_ready(d);
}

// The CO5300 refuses odd update windows. Without this the panel smears on
// partial redraws — text and diagonals worst. The vendor BSP does the same.
static void rounder_cb(lv_event_t *e) {
  lv_area_t *a = (lv_area_t *)lv_event_get_param(e);
  a->x1 = (a->x1 >> 1) << 1;
  a->y1 = (a->y1 >> 1) << 1;
  a->x2 = ((a->x2 >> 1) << 1) + 1;
  a->y2 = ((a->y2 >> 1) << 1) + 1;
}

// Poll the finger count rather than trusting the interrupt flag alone —
// the flag only pulses on a new touch, so drags and flings never track.
// Raw FT3168 read (uncorrected). Returns true if a finger is down. The touch
// calibration app uses this to pair known screen targets with raw coordinates.
bool touch_raw(int32_t *x, int32_t *y) {
  if (FT3168->IIC_Read_Device_Value(Arduino_IIC_Touch::Value_Information::TOUCH_FINGER_NUMBER) <= 0)
    return false;
  *x = FT3168->IIC_Read_Device_Value(Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X);
  *y = FT3168->IIC_Read_Device_Value(Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y);
  return true;
}

static void touch_read(lv_indev_t *indev, lv_indev_data_t *data) {
  if (g_dark) { data->state = LV_INDEV_STATE_RELEASED; g_ui_touching = false; return; }  // inputs off in dark modes
  int32_t rx, ry;
  if (touch_raw(&rx, &ry)) {
    int32_t sx, sy;
    tc_apply(rx, ry, &sx, &sy);                 // affine calibration (offset/scale/flip)
    data->point.x = constrain(sx, 0, LCD_WIDTH - 1);
    data->point.y = constrain(sy, 0, LCD_HEIGHT - 1);
    data->state   = LV_INDEV_STATE_PRESSED;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
  g_ui_touching = (data->state == LV_INDEV_STATE_PRESSED);
}

// ---------------------------------------------------------------- system helpers
void buzz(uint16_t ms) {
  if (!g_haptics) return;                 // Settings can silence the motor
  digitalWrite(PIN_MOTOR, HIGH);
  delay(ms);
  digitalWrite(PIN_MOTOR, LOW);
}

// Panel brightness, driven by Settings and Torch. Kept in g_brightness so a
// screen can restore the previous level on exit.
void ui_set_brightness(uint8_t b) { g_brightness = b; gfx->setBrightness(b); }
uint8_t ui_get_brightness(void)   { return g_brightness; }

// Charge current selector for Settings (0.33C / 0.67C / 1C on the 300 mAh cell).
void ui_set_charge_ma(int ma) {
  if (!SYS.pmuOk) return;
  if      (ma <= 100) PMU.setChargerConstantCurr(XPOWERS_AXP2101_CHG_CUR_100MA);
  else if (ma <= 200) PMU.setChargerConstantCurr(XPOWERS_AXP2101_CHG_CUR_200MA);
  else                PMU.setChargerConstantCurr(XPOWERS_AXP2101_CHG_CUR_300MA);
}

// ---- dark / wake for Stealth Log + Cloak ----
// Screen off (brightness 0) and touch ignored, but the loop keeps running so the
// active scanner keeps sniffing and logging to SD. Panel is the big power draw, so
// this is the battery-saver wardrive state. BOOT wakes it.
void ui_enter_dark(void) {
  if (g_dark) return;
  g_wake_bri = g_brightness;
  g_dark = true;
  gfx->setBrightness(0);
}
void ui_wake(void) {
  if (!g_dark) return;
  g_dark = false;
  g_stealth = false;
  g_mode_cloak = false;                 // waking clears the dark modes (back to normal)
  ui_set_brightness(g_wake_bri ? g_wake_bri : 0xD0);
  lv_obj_invalidate(lv_screen_active());  // force a full repaint on wake
}
bool ui_is_dark(void) { return g_dark; }

uint8_t hour12(uint8_t h24, const char **ampm) {
  if (ampm) *ampm = (h24 >= 12) ? "PM" : "AM";
  uint8_t h = h24 % 12;
  return h ? h : 12;
}

void sys_refresh(void) {
  if (SYS.pmuOk) {
    SYS.battVolt = PMU.getBattVoltage();
    SYS.battPct  = PMU.getBatteryPercent();
    SYS.charging = PMU.isCharging();
    SYS.vbus     = PMU.isVbusIn();
    SYS.vbusVolt = PMU.getVbusVoltage();
    SYS.sysVolt  = PMU.getSystemVoltage();
    // Percentage is voltage-derived and jumps on load changes; clamp the junk.
    if (SYS.battPct < 0)   SYS.battPct = 0;
    if (SYS.battPct > 100) SYS.battPct = 100;
  }
  if (SYS.rtcOk) {
    RTC_DateTime t = RTC.getDateTime();
    SYS.hh = t.getHour(); SYS.mm = t.getMinute(); SYS.ss = t.getSecond();
    SYS.year = t.getYear(); SYS.month = t.getMonth(); SYS.day = t.getDay();
  }
}

bool sys_imu_read(float *ax, float *ay, float *az, float *gx, float *gy, float *gz) {
  if (!SYS.imuOk) return false;
  if (!IMU.getDataReady()) return false;
  IMU.getAccelerometer(*ax, *ay, *az);
  IMU.getGyroscope(*gx, *gy, *gz);
  return true;
}

// Parse __DATE__ ("Sep 16 2026") and __TIME__ ("19:42:07") into an RTC_DateTime.
static bool build_datetime(RTC_DateTime &out) {
  static const char *MONTHS = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char mon[4] = {0};
  int d = 0, y = 0, hh = 0, mm = 0, ss = 0;
  if (sscanf(__DATE__, "%3s %d %d", mon, &d, &y) != 3) return false;
  if (sscanf(__TIME__, "%d:%d:%d", &hh, &mm, &ss) != 3) return false;
  const char *p = strstr(MONTHS, mon);
  if (!p) return false;
  int m = (int)((p - MONTHS) / 3) + 1;
  out = RTC_DateTime((uint16_t)y, (uint8_t)m, (uint8_t)d,
                     (uint8_t)hh, (uint8_t)mm, (uint8_t)ss);
  return true;
}

// Monotonic comparison key. Seconds since 2000-01-01, near enough for ordering.
static long date_key(uint16_t y, uint8_t mo, uint8_t d, uint8_t h, uint8_t mi, uint8_t s) {
  long days = ((long)y - 2000) * 365L + (mo * 31L) + d;
  return days * 86400L + h * 3600L + mi * 60L + s;
}

// ---------------------------------------------------------------- setup
void setup() {
  USBSerial.begin(115200);
  delay(200);
  USBSerial.println("\n[WEDJAT] boot");

  pinMode(PIN_MOTOR, OUTPUT);
  digitalWrite(PIN_MOTOR, LOW);
  pinMode(BTN_BOOT, INPUT_PULLUP);
  pinMode(BTN_PWR, INPUT_PULLDOWN);   // idle LOW so a floating pin can't self-trigger shutdown

  // Display first. No PMIC call is needed to light the panel — the AXP2101
  // comes up with its rails already on. Do NOT disable any ALDO: ALDO2 feeds
  // DSI_PWR_EN through R10 and turning it off blanks the screen.
  if (!gfx->begin()) USBSerial.println("[WEDJAT] gfx->begin() FAILED");
  gfx->fillScreen(RGB565_BLACK);
  gfx->setBrightness(0xD0);

  Wire.begin(IIC_SDA, IIC_SCL);
  Wire.setClock(400000);

  // Touch. Bounded retry with a reset pulse between attempts — the vendor
  // examples loop here forever on a black screen if the chip doesn't answer.
  bool tpOk = false;
  for (int attempt = 0; attempt < 5 && !tpOk; attempt++) {
    if (FT3168->begin()) { tpOk = true; break; }
    USBSerial.printf("[WEDJAT] FT3168 init failed (%d), pulsing reset\n", attempt + 1);
    pinMode(TP_RESET, OUTPUT);
    digitalWrite(TP_RESET, LOW);  delay(10);
    digitalWrite(TP_RESET, HIGH); delay(120);
  }
  if (tpOk) {
    FT3168->IIC_Write_Device_State(
        Arduino_IIC_Touch::Device::TOUCH_POWER_MODE,
        Arduino_IIC_Touch::Device_Mode::TOUCH_POWER_MONITOR);
    USBSerial.println("[WEDJAT] touch ok");
  } else {
    USBSerial.println("[WEDJAT] touch DEAD - continuing without it");
  }

  // PMIC. begin() is what the vendor's own example forgets to call.
  SYS.pmuOk = PMU.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (SYS.pmuOk) {
    PMU.disableTSPinMeasure();          // mandatory: no real NTC fitted, charging misbehaves otherwise
    PMU.enableBattVoltageMeasure();
    PMU.enableVbusVoltageMeasure();
    PMU.enableSystemVoltageMeasure();
    PMU.setChargerConstantCurr(XPOWERS_AXP2101_CHG_CUR_200MA);   // 0.67C on the 300mAh cell
    PMU.setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V2);
    // PWR key = hardware on/off. It powered ON but never OFF because only the off-TIME
    // was set -- long-press shutdown was never enabled and the long-press action was
    // never set to power-off. Do both: short press when off powers on, ~4s hold = off.
    PMU.setPowerKeyPressOnTime(XPOWERS_POWERON_128MS);
    PMU.setPowerKeyPressOffTime(XPOWERS_POWEROFF_4S);
    PMU.setLongPressPowerOFF();          // long-press action = power off (not restart)
    PMU.enableLongPressShutdown();       // <-- the missing enable that made "off" a no-op
    USBSerial.println("[WEDJAT] AXP2101 ok");
  } else {
    USBSerial.println("[WEDJAT] AXP2101 NOT FOUND");
  }

  // Powered up from USB while the watch was off (and a battery is present)? Don't
  // boot the full UI -- show the charge screen and wait for a PWR press.
  g_charge_mode = SYS.pmuOk && PMU.isVbusIn() && PMU.isBatteryConnect();
  USBSerial.printf("[WEDJAT] vbus=%d batt=%d -> %s\n",
                   SYS.pmuOk ? PMU.isVbusIn() : 0, SYS.pmuOk ? PMU.isBatteryConnect() : 0,
                   g_charge_mode ? "CHARGE screen" : "boot UI");

  // IMU — SA0 is tied low so the address is 0x6B. The rev 0.6 datasheet
  // Waveshare hosts has this inverted; rev 0.9 corrects it.
  SYS.imuOk = IMU.begin(Wire, QMI8658_L_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (SYS.imuOk) {
    IMU.configAccelerometer(SensorQMI8658::ACC_RANGE_4G,
                            SensorQMI8658::ACC_ODR_125Hz,
                            SensorQMI8658::LPF_MODE_0);
    IMU.configGyroscope(SensorQMI8658::GYR_RANGE_256DPS,
                        SensorQMI8658::GYR_ODR_112_1Hz,
                        SensorQMI8658::LPF_MODE_0);
    IMU.enableAccelerometer();
    IMU.enableGyroscope();
    USBSerial.println("[WEDJAT] QMI8658 ok");
  } else {
    USBSerial.println("[WEDJAT] QMI8658 NOT FOUND");
  }

  SYS.rtcOk = RTC.begin(Wire, IIC_SDA, IIC_SCL);
  if (SYS.rtcOk) {
    // Seed the clock from the build timestamp. __DATE__/__TIME__ are the
    // compiling machine's LOCAL time, and you build on the Vista box, so this
    // lands on Pacific automatically -- no timezone maths, no NTP, no WiFi.
    // Only applied when the RTC is behind the build, so a running clock that
    // has been keeping good time is never dragged backwards.
    RTC_DateTime now = RTC.getDateTime();
    RTC_DateTime bt;
    if (build_datetime(bt)) {
      long rtcKey   = date_key(now.getYear(), now.getMonth(), now.getDay(),
                               now.getHour(), now.getMinute(), now.getSecond());
      long buildKey = date_key(bt.getYear(), bt.getMonth(), bt.getDay(),
                               bt.getHour(), bt.getMinute(), bt.getSecond());
      if (now.getYear() < 2024 || rtcKey < buildKey) {
        RTC.setDateTime(bt);
        USBSerial.printf("[WEDJAT] RTC set from build: %04u-%02u-%02u %02u:%02u:%02u\n",
                         bt.getYear(), bt.getMonth(), bt.getDay(),
                         bt.getHour(), bt.getMinute(), bt.getSecond());
      }
    }
    USBSerial.println("[WEDJAT] PCF85063 ok");
  }

  sys_refresh();

  tc_begin();          // load touch calibration from SD (identity if none)

  // ---- LVGL ----
  lv_init();
  lv_tick_set_cb(millis_cb);

  // Partial mode, 60 rows. Two reasons over the vendor's DIRECT mode: it only
  // pushes dirty rectangles, and each transfer stays ~48KB — under the ~80KB
  // point where this board's QSPI transfers start failing.
  const uint32_t bufPx = LCD_WIDTH * 80;   // 80 rows so a zoomed home icon fits one flush band
  draw_buf = (lv_color_t *)heap_caps_malloc(bufPx * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!draw_buf) draw_buf = (lv_color_t *)heap_caps_malloc(bufPx * 2, MALLOC_CAP_SPIRAM);
  if (!draw_buf) { USBSerial.println("[WEDJAT] draw buffer alloc FAILED"); while (1) delay(1000); }

  disp = lv_display_create(LCD_WIDTH, LCD_HEIGHT);
  lv_display_set_flush_cb(disp, disp_flush);
  lv_display_set_buffers(disp, draw_buf, NULL, bufPx * 2, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_add_event_cb(disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);

  lv_indev_t *indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, touch_read);

  ui_init();
  USBSerial.printf("[WEDJAT] heap free %u internal / %u psram\n",
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  USBSerial.println("[WEDJAT] ready");
  buzz(40);
}

// ---------------------------------------------------------------- loop
void loop() {
  if (!g_dark) lv_task_handler();        // skip rendering while dark -> saves battery

  static uint32_t tTick = 0, tSys = 0, tChg = 0;
  uint32_t now = millis();

  if (now - tSys > 1000) { tSys = now; sys_refresh(); }

  if (g_charge_mode) {
    if (now - tChg > 1000) { tChg = now; ui_charge_tick(); }   // charge + show status only
    // BOOT also enters the UI here, as a fallback in case the PWR pin read is flaky.
    static bool cbWas = false;
    bool cbNow = (digitalRead(BTN_BOOT) == LOW);
    if (!cbNow && cbWas) { buzz(20); ui_charge_enter_ui(); }
    cbWas = cbNow;
  } else {
    if (now - tTick > 200) { tTick = now; ui_tick(); }         // normal UI (scanners tick even when dark)

    // BOOT: short press = wake (from dark) or home (in an app); hold ~2s = reboot.
    static bool bootWas = false;
    static uint32_t bootDownAt = 0;
    static bool bootRebooted = false;
    bool bootNow = (digitalRead(BTN_BOOT) == LOW);
    if (bootNow && !bootWas) { bootDownAt = now; bootRebooted = false; }
    if (bootNow && !bootRebooted && now - bootDownAt > 2000) {   // held -> reboot
      bootRebooted = true;
      buzz(200); delay(150);
      ESP.restart();
    }
    if (!bootNow && bootWas && !bootRebooted) {                  // released before 2s -> short press
      if (g_dark)                    { buzz(20); ui_wake(); }
      else if (ui_active_app() >= 0) { buzz(25); ui_go_home(); }
    }
    bootWas = bootNow;
  }

  // PWR button (both modes): short press on the charge screen enters the UI; a hold
  // powers the watch OFF via the PMIC (the AXP2101's own long-press wasn't reaching
  // the button, so firmware drives shutdown directly).
  static bool pwrWas = false;
  static uint32_t pwrDownAt = 0;
  static bool pwrHandled = false;
  bool pwrNow = (digitalRead(BTN_PWR) == HIGH);   // pin_config: HIGH = pressed (inverted)
  if (pwrNow && !pwrWas) { pwrDownAt = now; pwrHandled = false; USBSerial.println("[WEDJAT] PWR down"); }
  if (pwrNow && !pwrHandled && now - pwrDownAt > 1500) {   // hold -> power OFF
    pwrHandled = true;
    USBSerial.println("[WEDJAT] PWR hold -> shutdown");
    if (SYS.pmuOk) PMU.shutdown();
  }
  if (!pwrNow && pwrWas && !pwrHandled) {                  // short press
    if (g_charge_mode) { buzz(20); ui_charge_enter_ui(); }
  }
  pwrWas = pwrNow;

  delay(5);
}
