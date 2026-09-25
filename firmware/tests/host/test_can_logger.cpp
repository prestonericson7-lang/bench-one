// test_can_logger.cpp -- the CAN logger's real firmware: listen-only setup, identity, the raw-stream
// commands the hub depends on, and the SD log it writes (checked afterwards by canlog.py's reader).
#include "host_globals.h"
#include <SD.h>
#include <FlexCAN_T4.h>
HostCanBus host_can[4];
std::map<std::string, std::vector<uint8_t>> host_files;
bool host_sd_ok = true;
SDClass SD;
#include "../../car-can-logger/car-can-logger.ino"

static void frame(int bus, uint32_t id, std::vector<uint8_t> data, bool ext = false) {
  CAN_message_t m; m.bus = bus; m.id = id; m.len = data.size(); m.flags.extended = ext;
  for (size_t i = 0; i < data.size(); i++) m.buf[i] = data[i];
  host_us += 137; host_ms = host_us / 1000;
  host_can[bus].cb(m);
}
static void run(int n = 3) { for (int i = 0; i < n; i++) { loop(); host_us += 1000; host_ms = host_us / 1000; } }

int main() {
  std::printf("car-can-logger (real firmware on the host)\n");
  setup();
  std::string boot = drain(Serial, g_log_usb);
  CHECK(host_can[1].mode == LISTEN_ONLY && host_can[2].mode == LISTEN_ONLY, "both buses configured LISTEN_ONLY (cannot transmit, cannot ACK)");
  CHECK(host_can[1].baud == 500000 && host_can[2].baud == 100000, "bus1 500 kbit/s, bus2 100 kbit/s (config.h)");
  CHECK(host_can[1].cb && host_can[2].cb, "receive callbacks installed on both buses");
  CHECK(boot.find("log open: CAN_0001.BIN") != std::string::npos, "log file opened on boot");

  Serial.feed("I"); run();
  CHECK(drain(Serial, g_log_usb).find("ID=car-can-logger") != std::string::npos, "'I' -> ID=car-can-logger (how the hub finds it)");

  frame(1, 0x1A0, {0xC8, 0x00, 0x5A, 0x00}); run();
  CHECK(drain(Serial, g_log_usb).find("R,") == std::string::npos, "stream OFF by default: no R lines");

  Serial.feed("R"); run(); drain(Serial, g_log_usb);
  frame(1, 0x1A0, {0xC8, 0x00, 0x5A, 0x00});
  frame(2, 0x18DAF110, {0x02, 0x10, 0x03}, true);
  frame(1, 0x7, {}); run();
  std::string s = drain(Serial, g_log_usb);
  CHECK(s.find(",1,1A0,C8005A00\r\n") != std::string::npos, "'R' -> R,<t>,1,1A0,C8005A00");
  CHECK(s.find(",2,18DAF110,021003\r\n") != std::string::npos, "extended ID on bus 2 streamed in upper hex");
  CHECK(s.find(",1,7,\r\n") != std::string::npos, "zero-length frame streamed with an empty data field");

  Serial.feed("R"); run(); frame(1, 0x100, {1}); run();
  CHECK(drain(Serial, g_log_usb).find(",1,100,01") != std::string::npos, "'R' again keeps it ON (idempotent: a hub restart cannot switch it off)");
  Serial.feed("X"); run(); drain(Serial, g_log_usb); frame(1, 0x100, {1}); run();
  CHECK(drain(Serial, g_log_usb).find("R,") == std::string::npos, "'X' -> stream OFF");
  Serial.feed("r"); run(); drain(Serial, g_log_usb); frame(1, 0x101, {2}); run();
  CHECK(drain(Serial, g_log_usb).find(",1,101,02") != std::string::npos, "'r' still toggles for a human at a terminal");

  Serial.feed("f"); run();
  auto &bin = host_files["CAN_0001.BIN"];
  CHECK(bin.size() > 16 && std::string(bin.begin(), bin.begin() + 8) == "CANLOG2\n", "SD log starts with the CANLOG2 header");
  dump("CAN_0001.BIN", std::string(bin.begin(), bin.end()));

  host_ms += 20000; host_us = host_ms * 1000; run();
  CHECK(drain(Serial, g_log_usb).find("log closed") != std::string::npos, "bus idle 15 s -> log closed (sleeps with the car)");

  dump("can_logger_usb.txt", g_log_usb);
  std::printf("%d checks, %d failed\n", g_checks, g_fails);
  return g_fails ? 1 : 0;
}
