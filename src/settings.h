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
};

Settings loadSettings();
void saveSettings(const Settings &s);
/// Wipe ONLY the saved WiFi creds (ssid/pass -> empty); name + MQTT kept.
/// (Console `prov` / web "Provision via BLE" call this, then reboot.)
void clearWiFiCreds();
