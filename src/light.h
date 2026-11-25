#pragma once
// Shared light data model (used by main.cpp and the web handlers).
#include <Arduino.h>
#include <string>
#include <vector>
#include "config.h"

// The HA bridge (HaBridge) publishes from this struct directly — no per-light
// handle is stored here. The bridge builds its discovery/state JSON from
// `id`/`name`/`number` + the believed state, so this model stays lib-free.
struct LightDevice {
  std::string address;              // BLE MAC address (identity + logging)
  std::vector<uint8_t> mfgData;     // raw 18-byte manufacturer data (used to key)
  uint8_t type[2] = { 0, 0 };       // light-type code (e.g. A8A1 = RGBW)
  bool isRegistered = false;        // true once it holds our key
  bool needsKey = false;            // transient: a factory identity was seen — key it (not persisted)
  std::string id;                   // HA unique id
  std::string name;                 // HA friendly name
  uint8_t number = 0;               // light number we assigned it

  // ── Believed state (FR-05) ──────────────────────────────────────────────
  // The bulbs are broadcast-only (no readback), so the last intent we *sent*
  // IS the source of truth. Tracked per bulb so (a) the UI restores it on
  // refresh/reboot instead of a white box, and (b) the re-transmit loop (P2.9)
  // can re-assert it. Defaults = "on, full white" (a freshly-keyed bulb).
  bool    on        = true;
  uint8_t brightness = 127;         // 0-127
  uint8_t rgbR      = 255, rgbG = 255, rgbB = 255;
  uint16_t ct       = 4000;         // colour temp in Kelvin, 2700 (warm) → 6000 (cool); the white LED (RGBW), split into the WW/CW pair only at TX
  uint8_t colorMode = 0;            // 0 = colour unset, 1 = rgb, 2 = ct
};
