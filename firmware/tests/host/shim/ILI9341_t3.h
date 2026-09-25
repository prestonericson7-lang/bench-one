// ILI9341_t3.h -- host stand-in: the vent display's drawing calls do nothing (the test checks the
// telemetry the firmware parsed, not pixels).
#pragma once
#include <Arduino.h>
#define ILI9341_BLACK 0x0000
#define ILI9341_WHITE 0xFFFF
class ILI9341_t3 {
 public:
  ILI9341_t3(int, int, int = 255, int = 11, int = 13, int = 12) {}
  void begin() {}
  void setRotation(int) {}
  void fillScreen(uint16_t) {}
  void fillRect(int, int, int, int, uint16_t) {}
  void setTextColor(uint16_t) {}
  void setTextColor(uint16_t, uint16_t) {}
  void setTextSize(int) {}
  void setCursor(int, int) {}
  int width() const { return 320; }
  int height() const { return 240; }
  template <typename T> void print(T) {}
  template <typename T> void print(T, int) {}
  template <typename T> void println(T) {}
  static uint16_t color565(uint8_t r, uint8_t g, uint8_t b) { return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }
};
