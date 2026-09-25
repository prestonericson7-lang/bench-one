// host_globals.h -- the shim's state, defined once per test program (each test is one translation
// unit that #includes one firmware .ino after this file).
#pragma once
#include <Arduino.h>
#include <string>
#include <vector>
uint32_t host_ms = 0, host_us = 0;
std::map<int, int> host_adc, host_din;
std::map<int, uint32_t> host_ledc;
HostSerial Serial, Serial1;
long host_enc = 0;

static int g_checks = 0, g_fails = 0;
#define CHECK(cond, what)                                                                         \
  do {                                                                                            \
    g_checks++;                                                                                   \
    if (cond) std::printf("  ok    %s\n", what);                                                  \
    else { g_fails++; std::printf("  FAIL  %s   (%s:%d)\n", what, __FILE__, __LINE__); }          \
  } while (0)

// every byte the firmware sent on a port during the whole run, for the Python cross-check
static std::string g_log_usb, g_log_link;
static std::string drain(HostSerial &p, std::string &log) { std::string s = p.take(); log += s; return s; }
static void dump(const char *path, const std::string &s) {
  FILE *f = std::fopen(path, "wb"); if (f) { std::fwrite(s.data(), 1, s.size(), f); std::fclose(f); }
}
