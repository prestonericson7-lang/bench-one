// FlexCAN_T4.h -- host stand-in: records the configuration (so the test can assert LISTEN_ONLY) and
// keeps the onReceive callback so the test can deliver frames exactly as the interrupt would.
#pragma once
#include <Arduino.h>
enum CAN_DEV_TABLE { CAN1 = 1, CAN2 = 2, CAN3 = 3 };
enum FLEXCAN_RXQUEUE_TABLE { RX_SIZE_256 = 256 };
enum FLEXCAN_TXQUEUE_TABLE { TX_SIZE_16 = 16 };
enum FLEXCAN_MODE { LISTEN_ONLY = 1, NORMAL = 0 };
struct CAN_message_t {
  uint32_t id = 0;
  uint16_t timestamp = 0;
  uint8_t idhit = 0;
  struct { bool extended = false; bool remote = false; bool overrun = false; bool reserved = false; } flags;
  uint8_t len = 8;
  uint8_t buf[8] = {0};
  int8_t mb = 0;
  uint8_t bus = 0;
  bool seq = false;
};
typedef void (*_MB_ptr)(const CAN_message_t &msg);
struct HostCanBus { uint32_t baud = 0; int mode = -1; _MB_ptr cb = nullptr; bool begun = false; };
extern HostCanBus host_can[4];
template <CAN_DEV_TABLE BUS, FLEXCAN_RXQUEUE_TABLE RX, FLEXCAN_TXQUEUE_TABLE TX>
class FlexCAN_T4 {
 public:
  void begin() { host_can[BUS].begun = true; }
  void setBaudRate(uint32_t b, FLEXCAN_MODE m = NORMAL) { host_can[BUS].baud = b; host_can[BUS].mode = m; }
  void setMaxMB(int) {}
  void enableFIFO(bool = true) {}
  void enableFIFOInterrupt(bool = true) {}
  void onReceive(_MB_ptr cb) { host_can[BUS].cb = cb; }
  int events() { return 0; }
};
