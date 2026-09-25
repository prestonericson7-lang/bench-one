// SD.h -- host stand-in: files live in memory; the test can inspect what the logger wrote.
#pragma once
#include <Arduino.h>
#include <vector>
#define BUILTIN_SDCARD 254
#define FILE_WRITE 1
#define FIFO_SDIO 0
struct SdioConfig { explicit SdioConfig(int) {} };
extern std::map<std::string, std::vector<uint8_t>> host_files;
extern bool host_sd_ok;
class File {
 public:
  std::string name;
  bool open_ = false;
  File() {}
  explicit File(const std::string &n) : name(n), open_(true) {}
  size_t write(const uint8_t *p, size_t n) { auto &v = host_files[name]; v.insert(v.end(), p, p + n); return n; }
  size_t write(const char *s) { return write((const uint8_t *)s, strlen(s)); }
  void flush() {}
  void close() { open_ = false; }
  operator bool() const { return open_; }
};
struct HostSdFs { bool begin(SdioConfig) { return host_sd_ok; } };
class SDClass {
 public:
  HostSdFs sdfs;
  bool begin(int) { return host_sd_ok; }
  bool exists(const char *n) { return host_files.count(n) > 0; }
  File open(const char *n, int) { host_files[n]; return File(n); }
};
extern SDClass SD;
