// test_climate_node.cpp -- the climate node's real firmware: control law, fail-safe, and the
// KEY=value telemetry the hub parses.
#include "host_globals.h"
#include "../../vent-climate-node/vent-climate-node.ino"

// ADC count the firmware's own divider + beta model maps to tC (inverse of readTempC)
static int adc_for(float tC) {
  float r = NTC_R25 * expf(NTC_BETA * (1.0f / (tC + 273.15f) - 1.0f / 298.15f));
  return (int)lroundf(ADC_MAX * r / (R_FIXED + r));
}
static void run_ms(uint32_t ms) { uint32_t end = host_ms + ms; while (host_ms < end) loop(); }  // loop() delays 10 ms itself
static std::string last_value(const std::string &out, const char *key) {
  std::string k = std::string(key) + "=", v; size_t p = 0;
  while ((p = out.find(k, p)) != std::string::npos) {
    if (p == 0 || out[p - 1] == '\n') { size_t e = out.find_first_of("\r\n", p); v = out.substr(p + k.size(), e - p - k.size()); }
    p += k.size();
  }
  return v;
}

int main() {
  std::printf("vent-climate-node (real firmware on the host)\n");
  host_adc[PIN_T_AIR] = adc_for(15.0f); host_adc[PIN_T_CASE] = adc_for(35.0f);
  setup();
  std::string boot = drain(Serial, g_log_usb);
  CHECK(host_ledc.count(PIN_SERVO) && host_ledc[PIN_SERVO] > 0, "servo driven at boot (to CLOSED, the fail-safe)");
  bool all_hash = true; size_t p = 0;
  while (p < boot.size()) { size_t e = boot.find('\n', p); std::string ln = boot.substr(p, e - p); if (!ln.empty() && ln[0] != '#' && ln != "\r") all_hash = false; p = e + 1; }
  CHECK(all_hash, "boot banner lines all start with '#': prose can never become a hub key");

  Serial.feed("ID?\n"); run_ms(30);
  CHECK(drain(Serial, g_log_usb).find("ID=climate-node") != std::string::npos, "'ID?' -> ID=climate-node");

  run_ms(3000);                                            // cold air: AC on
  std::string out = drain(Serial, g_log_usb);
  float a = atof(last_value(out, "AIRT").c_str()), c = atof(last_value(out, "CASET").c_str());
  CHECK(fabsf(a - 15.0f) < 0.3f && fabsf(c - 35.0f) < 0.3f, "AIRT/CASET reported from the thermistors (15.0 / 35.0 C)");
  CHECK(last_value(out, "DAMPER") == "OPEN", "cold air -> DAMPER=OPEN (free cooling)");
  CHECK(last_value(out, "CLIMATE_FAULT") == "0" && last_value(out, "CLIMATE_MODE") == "auto", "CLIMATE_FAULT=0, CLIMATE_MODE=auto");

  host_adc[PIN_T_AIR] = adc_for(24.0f); run_ms(3000);      // inside the hysteresis band: hold
  CHECK(last_value(drain(Serial, g_log_usb), "DAMPER") == "OPEN", "24 C (between 18 and 30): holds OPEN -- no hunting");

  host_adc[PIN_T_AIR] = adc_for(35.0f); run_ms(3000);      // heat on
  CHECK(last_value(drain(Serial, g_log_usb), "DAMPER") == "CLOSED", "hot air -> DAMPER=CLOSED (protect the stack)");

  host_adc[PIN_T_AIR] = 0; run_ms(3000);                   // open-circuit sensor
  out = drain(Serial, g_log_usb);
  CHECK(last_value(out, "AIRT") == "nan" && last_value(out, "CLIMATE_FAULT") == "1", "sensor fault -> AIRT=nan, CLIMATE_FAULT=1");
  CHECK(last_value(out, "DAMPER") == "CLOSED", "sensor fault -> DAMPER=CLOSED (fail-safe)");

  host_adc[PIN_T_AIR] = adc_for(10.0f); run_ms(3000);
  host_ms += 2000; run_ms(100);
  CHECK(host_ledc.count(PIN_SERVO) == 0 || host_ledc[PIN_SERVO] == 0, "servo released after the move settles (no stall current)");

  Serial.feed("o"); run_ms(1200);
  out = drain(Serial, g_log_usb);
  CHECK(last_value(out, "CLIMATE_MODE") == "manual" && last_value(out, "DAMPER") == "OPEN", "'o' -> manual OPEN, reported as CLIMATE_MODE=manual");

  dump("climate_node_usb.txt", g_log_usb);
  std::printf("%d checks, %d failed\n", g_checks, g_fails);
  return g_fails ? 1 : 0;
}
