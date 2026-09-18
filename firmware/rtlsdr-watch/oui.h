// Compact OUI -> vendor lookup + device-type guessing.
// Not exhaustive (that's a 30k-row DB); this covers the makers you actually see
// walking around. Unknown OUIs fall back to "unknown"; locally-administered
// addresses (randomised MACs, common on modern phones) are flagged as such.
#pragma once
#include <Arduino.h>

struct oui_ent_t { uint32_t oui; const char *vendor; };

// oui = first 3 bytes packed big-endian (aa:bb:cc -> 0xAABBCC)
static const oui_ent_t OUI_TABLE[] = {
  {0x001451,"Apple"},{0x0017F2,"Apple"},{0x0025BC,"Apple"},{0x3C0754,"Apple"},
  {0xA85C2C,"Apple"},{0xF0989D,"Apple"},{0xDC2B2A,"Apple"},{0x8866A5,"Apple"},
  {0x0021CC,"Samsung"},{0x0026E8,"Samsung"},{0x5CF6DC,"Samsung"},{0xE8508B,"Samsung"},
  {0x94350A,"Samsung"},{0x000FDE,"Sony"},{0xFCF152,"Sony"},{0x001A11,"Google"},
  {0x3C5AB4,"Google"},{0xF4F5E8,"Google"},{0xF88FCA,"Google"},{0x94EB2C,"Google"},
  {0x24629F,"Espressif"},{0x7CDFA1,"Espressif"},{0xA0764E,"Espressif"},{0x083AF2,"Espressif"},
  {0x3C71BF,"Espressif"},{0x8CAAB5,"Espressif"},{0xB827EB,"RaspberryPi"},{0xDCA632,"RaspberryPi"},
  {0xE45F01,"RaspberryPi"},{0x001A79,"Ubiquiti"},{0x24A43C,"Ubiquiti"},{0x788A20,"Ubiquiti"},
  {0x0018E7,"Cisco"},{0x00256C,"Cisco"},{0xC4E90A,"Cisco"},{0x00037F,"Atheros"},
  {0x001B63,"Intel"},{0x34E6D7,"Intel"},{0x8C5477,"Intel"},{0xA0A8CD,"Intel"},
  {0x0050F2,"Microsoft"},{0x000D3A,"Microsoft"},{0x7C1E52,"Microsoft"},{0x001167,"Amazon"},
  {0x44650D,"Amazon"},{0xFCA667,"Amazon"},{0x8871E5,"Amazon"},{0x0071CC,"Netgear"},
  {0x2C3033,"Netgear"},{0xA040A0,"Netgear"},{0x00904C,"TP-Link"},{0x50C7BF,"TP-Link"},
  {0x1C61B4,"TP-Link"},{0xEC086B,"TP-Link"},{0x00184D,"Netgear"},{0x18B430,"Nest"},
  {0x641666,"Tesla"},{0x4CFCAA,"Tesla"},{0x54D2E4,"Tuya"},{0xD8F15B,"Espressif"},
};

inline const char *oui_vendor(const uint8_t *mac) {
  // Locally administered bit (bit 1 of first octet) -> randomised / private MAC
  if (mac[0] & 0x02) return "private";
  uint32_t k = ((uint32_t)mac[0] << 16) | ((uint32_t)mac[1] << 8) | mac[2];
  for (unsigned i = 0; i < sizeof(OUI_TABLE)/sizeof(OUI_TABLE[0]); i++)
    if (OUI_TABLE[i].oui == k) return OUI_TABLE[i].vendor;
  return "unknown";
}

// Rough device class from vendor + radio it was seen on.
inline const char *device_kind(const char *vendor, bool isBle) {
  if (!strcmp(vendor,"Apple"))       return isBle ? "iPhone/Watch/AirPods" : "Apple device";
  if (!strcmp(vendor,"Samsung"))     return "Samsung phone/TV";
  if (!strcmp(vendor,"Google"))      return "Pixel/Nest";
  if (!strcmp(vendor,"Amazon"))      return "Echo/FireTV";
  if (!strcmp(vendor,"Tesla"))       return "Tesla";
  if (!strcmp(vendor,"Espressif"))   return "ESP32/IoT";
  if (!strcmp(vendor,"RaspberryPi")) return "Raspberry Pi";
  if (!strcmp(vendor,"Nest"))        return "Nest device";
  if (!strcmp(vendor,"Tuya"))        return "smart-home";
  if (!strcmp(vendor,"Ubiquiti") || !strcmp(vendor,"Cisco") ||
      !strcmp(vendor,"Netgear")  || !strcmp(vendor,"TP-Link")) return "network gear";
  if (!strcmp(vendor,"Intel"))       return "laptop/PC";
  if (!strcmp(vendor,"private"))     return isBle ? "phone (private addr)" : "device (private addr)";
  return isBle ? "BLE device" : "WiFi device";
}
