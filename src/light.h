#pragma once
// Shared light data model (used by main.cpp and the web handlers).
#include <Arduino.h>
#include <string>
#include <vector>
#include "config.h"

// Phase 3 will add the HALight* handle here (home-assistant-integration);
// keep the data model free of the library until then.
struct LightDevice {
  std::string address;              // BLE MAC address (identity + logging)
  std::vector<uint8_t> mfgData;     // raw 18-byte manufacturer data (used to key)
  uint8_t type[2] = { 0, 0 };       // light-type code (e.g. A8A1 = RGBW)
  bool isRegistered = false;        // true once it holds our key
  std::string id;                   // HA unique id
  std::string name;                 // HA friendly name
  uint8_t number = 0;               // light number we assigned it
};
