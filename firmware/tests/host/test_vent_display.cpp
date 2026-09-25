// test_vent_display.cpp -- the vent display's real firmware, its telemetry input on USB and UART.
#include "host_globals.h"
#include "../../vent-display/vent-display.ino"

static void run_ms(uint32_t ms) { for (uint32_t t = 0; t < ms; t += 5) { loop(); delay(5); } }

int main() {
  std::printf("vent-display (real firmware on the host)\n");
  setup();
  drain(Serial, g_log_usb);

  Serial.feed("ID?\n"); run_ms(20);
  CHECK(drain(Serial, g_log_usb).find("ID=vent-display") != std::string::npos, "USB 'ID?' -> ID=vent-display (how the hub finds it)");
  Serial1.feed("ID?\n"); run_ms(20);
  CHECK(drain(Serial1, g_log_link).find("ID=vent-display") != std::string::npos, "UART 'ID?' -> answered on the UART");

  // exactly what hub.py's DisplayWriter sends (display_lines)
  Serial.feed("SOC=64.5\nAIRT=17.2\nCASET=38.5\nDAMPER=OPEN\nSTAT=OK\n"); run_ms(20);
  CHECK(T.soc == 64.5f && T.airT == 17.2f && T.caseT == 38.5f, "USB telemetry parsed: SOC 64.5, AIRT 17.2, CASET 38.5");
  CHECK(!strcmp(T.damper, "OPEN") && !strcmp(T.stat, "OK"), "USB telemetry parsed: DAMPER OPEN, STAT OK");
  uint32_t rx = T.lastRx;
  CHECK(rx > 0, "lastRx stamped (drives the LINK indicator)");

  Serial1.feed("VOLT=14.1\r\nSTAT=FLT climate-nod\r\n"); run_ms(20);
  CHECK(T.volt == 14.1f && !strcmp(T.stat, "FLT climate-nod"), "UART telemetry (the car link) parsed, CRLF tolerated");

  Serial.feed("NOTAKEY\nX=1\n"); run_ms(20);
  std::string lng(100, 'A'); lng += "=1\n";
  Serial.feed(lng); Serial.feed("SOC=70\n"); run_ms(20);
  CHECK(T.soc == 70.0f, "junk, unknown keys and an overlong line dropped; the next good line still applies");

  Serial.feed("SOC=7"); run_ms(20); Serial1.feed("AIRT=5\n"); run_ms(20); Serial.feed("1\n"); run_ms(20);
  CHECK(T.soc == 71.0f && T.airT == 5.0f, "USB and UART lines assemble independently (no cross-talk)");

  // exercise every page's drawing path with the encoder and the stale path with time
  for (int p = 0; p < 6; p++) { host_enc += 4; run_ms(150); }
  host_ms += 5000; run_ms(150);
  CHECK(millis() - T.lastRx > 3000, "stale after 3 s without telemetry (display shows LINK)");

  dump("vent_display_usb.txt", g_log_usb);
  std::printf("%d checks, %d failed\n", g_checks, g_fails);
  return g_fails ? 1 : 0;
}
