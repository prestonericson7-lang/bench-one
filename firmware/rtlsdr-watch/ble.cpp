#include "ble.h"
#include <BLEDevice.h>

static bool     inited = false;
static BLEScan *g_scan = NULL;

void ble_ensure_init(void) {
  if (inited) return;
  BLEDevice::init("");                 // controller + host up, stays up for good
  g_scan = BLEDevice::getScan();
  inited = true;
}

BLEScan *ble_scan(void) { ble_ensure_init(); return g_scan; }

void ble_scan_start(BLEAdvertisedDeviceCallbacks *cb, bool active, uint16_t itvl, uint16_t win) {
  ble_ensure_init();
  g_scan->setAdvertisedDeviceCallbacks(cb, true);
  g_scan->setActiveScan(active);
  g_scan->setInterval(itvl);
  g_scan->setWindow(win);
  g_scan->start(0, nullptr, false);    // 0 = scan continuously until stopped
}

void ble_scan_stop(void) {
  if (!inited || !g_scan) return;
  g_scan->stop();
  g_scan->clearResults();              // free the result list, but NOT the stack
}
