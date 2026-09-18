// Shared BLE controller. The Arduino BLE stack crashes if you init/deinit the
// controller repeatedly (which is exactly what happens when you bounce between
// radio apps). So we bring the controller up ONCE and never tear it down for
// the rest of the session -- apps only start/stop the *scan*, never the stack.
#pragma once
#include <vector>          // BLEAdvertisedDevice.h uses std::vector but forgets to include it
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

void     ble_ensure_init(void);                 // bring the controller up (once)
BLEScan *ble_scan(void);                         // shared scanner (controller up)
void     ble_scan_start(BLEAdvertisedDeviceCallbacks *cb,
                        bool active = true, uint16_t itvl = 100, uint16_t win = 80);
void     ble_scan_stop(void);                    // stop scanning, keep controller
