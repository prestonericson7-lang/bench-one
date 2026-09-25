// Arduino.h -- host stand-in for the Arduino/Teensy/ESP32 core, just enough to run the car firmwares'
// real .ino code on a PC. Serial ports are byte queues the test fills and drains; time is a variable
// the test advances; ADC readings come from a table the test sets. Nothing here is "simulated
// physics" -- it only lets the firmware's own logic and its USB/UART protocol run and be checked.
#pragma once
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <string>
using std::isnan;   // the Arduino cores expose isnan/isinf at global scope (math.h)
using std::isinf;

#define HEX 16
#define DEC 10
#define HIGH 1
#define LOW 0
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2
#define DMAMEM
#define FLASHMEM
#define PROGMEM
#define F(s) (s)

extern uint32_t host_ms, host_us;
inline uint32_t millis() { return host_ms; }
inline uint32_t micros() { return host_us; }
inline void delay(uint32_t ms) { host_ms += ms; host_us += ms * 1000; }
inline void delayMicroseconds(uint32_t us) { host_us += us; }
inline void yield() {}

extern std::map<int, int> host_adc;          // pin -> raw ADC count
extern std::map<int, int> host_din;          // pin -> digital level (default HIGH)
inline int analogRead(int pin) { return host_adc.count(pin) ? host_adc[pin] : 2048; }
inline void analogReadResolution(int) {}
inline int digitalRead(int pin) { return host_din.count(pin) ? host_din[pin] : HIGH; }
inline void digitalWrite(int, int) {}
inline void pinMode(int, int) {}
extern std::map<int, uint32_t> host_ledc;    // pin -> last duty written
inline bool ledcAttach(int, uint32_t, uint8_t) { return true; }
inline bool ledcDetach(int pin) { host_ledc.erase(pin); return true; }
inline bool ledcWrite(int pin, uint32_t duty) { host_ledc[pin] = duty; return true; }

class String {
 public:
  std::string s;
  String(const char *c = "") : s(c) {}
  String(unsigned long v) : s(std::to_string(v)) {}
  String(long v) : s(std::to_string(v)) {}
  String(int v) : s(std::to_string(v)) {}
  const char *c_str() const { return s.c_str(); }
};

class Print {
 public:
  virtual ~Print() {}
  virtual size_t write(uint8_t b) = 0;
  size_t write(const uint8_t *p, size_t n) { for (size_t i = 0; i < n; i++) write(p[i]); return n; }
  size_t print(const char *s) { size_t n = 0; while (*s) n += write((uint8_t)*s++); return n; }
  size_t print(const String &s) { return print(s.c_str()); }
  size_t print(char c) { return write((uint8_t)c); }
  size_t print(unsigned long v, int base = DEC) { return printNum(v, base); }
  size_t print(long v, int base = DEC) {
    if (base == DEC && v < 0) { write('-'); return 1 + printNum((unsigned long)(-v), DEC); }
    return printNum((unsigned long)v, base);
  }
  size_t print(unsigned int v, int base = DEC) { return printNum(v, base); }
  size_t print(int v, int base = DEC) { return print((long)v, base); }
  size_t print(unsigned char v, int base = DEC) { return printNum(v, base); }
  size_t print(double v, int digits = 2) { char b[48]; snprintf(b, sizeof b, "%.*f", digits, v); return print(b); }
  size_t println() { return print("\r\n"); }
  template <typename T> size_t println(T v) { size_t n = print(v); return n + println(); }
  template <typename T> size_t println(T v, int b) { size_t n = print(v, b); return n + println(); }
  size_t printf(const char *fmt, ...) __attribute__((format(printf, 2, 3))) {
    char b[512]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap); return print(b);
  }
 private:
  size_t printNum(unsigned long v, int base) {
    char b[40]; if (base == HEX) snprintf(b, sizeof b, "%lX", v); else snprintf(b, sizeof b, "%lu", v); return print(b);
  }
};

class Stream : public Print {
 public:
  virtual int available() = 0;
  virtual int read() = 0;
};

// A serial port: the firmware reads `in`, writes `out`; the test does the opposite.
class HostSerial : public Stream {
 public:
  std::deque<uint8_t> in;
  std::string out;
  void begin(unsigned long) {}
  int available() override { return (int)in.size(); }
  int read() override { if (in.empty()) return -1; int c = in.front(); in.pop_front(); return c; }
  size_t write(uint8_t b) override { out.push_back((char)b); return 1; }
  void flush() {}
  operator bool() const { return true; }
  void feed(const std::string &s) { for (char c : s) in.push_back((uint8_t)c); }
  std::string take() { std::string r; r.swap(out); return r; }
};
extern HostSerial Serial, Serial1;
