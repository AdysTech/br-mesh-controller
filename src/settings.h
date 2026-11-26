#pragma once
// ============================================================================
//  Runtime settings (NVS-backed).
//
//  The web UI (and later the captive-portal /setup page) reads + writes these.
//  Defaults come from config.h; anything saved in NVS wins.
// ============================================================================
#include <string>
#include <stdint.h>

struct Settings {
  std::string name;      // controller name (UI title, mDNS hostname)
  std::string ssid;      // empty = WiFi not configured yet -> AP mode
  std::string pass;
  std::string mqttHost;  // broker address
  uint16_t    mqttPort;
  std::string mqttUser;  // optional broker auth (empty = none)
  std::string mqttPass;
  std::string mqttClient; // MQTT client id (broker-side name); empty = derived brmesh-<MAC tail>
  std::string mqttName;  // HA device name for the controller hub (dev.name; empty = use `name`)
  int         scanSec;   // discovery window (seconds); the default every scan path uses.
                         // NVS "scandur"; seed SCAN_DURATION (config.h). SPA Config +
                         // HA scan button + console `scan` all read this one value.
};

Settings loadSettings();
void saveSettings(const Settings &s);
/// Ensure the optional Phase-3 NVS keys exist (as empty values). Call once at
/// boot — this core's Preferences::getString() logs an ESP_ERROR for every
/// missing key, so an unsaved key would spam serial on every /api/status poll.
void seedSettings();
/// Wipe ONLY the saved WiFi creds (ssid/pass -> empty); name + MQTT kept.
/// (Console `prov` / web "Provision via BLE" call this, then reboot.)
void clearWiFiCreds();
