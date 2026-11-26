// NVS-backed runtime settings (see settings.h).
// Lives in the same "brmesh" namespace as the mesh key + light list.
#include "settings.h"
#include <Arduino.h>
#include <Preferences.h>
#include <nvs.h>
#include "config.h"
#include "web/WebLog.h"   // leveled logger (P2.11)

static Preferences s_prefs;

// The core's Preferences getters log an ESP_ERROR for a key that was never
// written, and /api/status (pollable) runs loadSettings() — so a board whose
// MQTT section was never saved would spam serial forever. Raw NVS calls do
// not log; write the four optional keys empty, once.
void seedSettings() {
  nvs_handle_t h;
  if (nvs_open("brmesh", NVS_READWRITE, &h) != ESP_OK) return;
  char tmp[16];
  for (const char* k : {"muser", "mpass", "mclient", "mname"}) {
    size_t len = sizeof(tmp);
    if (nvs_get_str(h, k, tmp, &len) != ESP_OK)
      nvs_set_str(h, k, "");
  }
  nvs_commit(h);
  nvs_close(h);
}

Settings loadSettings() {
  Settings s;
  s.name     = DEFAULT_NAME;
  s.ssid     = DEFAULT_WIFI_SSID;
  s.pass     = DEFAULT_WIFI_PASS;
  s.mqttHost = DEFAULT_MQTT_HOST;
  s.mqttPort = DEFAULT_MQTT_PORT;

  s_prefs.begin("brmesh", false);
  s.name     = s_prefs.getString("name", s.name.c_str()).c_str();
  s.ssid     = s_prefs.getString("ssid", s.ssid.c_str()).c_str();
  s.pass     = s_prefs.getString("pass", s.pass.c_str()).c_str();
  s.mqttHost = s_prefs.getString("mhost", s.mqttHost.c_str()).c_str();
  s.mqttPort = (uint16_t)s_prefs.getInt("mport", s.mqttPort);
  s.mqttUser = s_prefs.getString("muser", "").c_str();
  s.mqttPass = s_prefs.getString("mpass", "").c_str();
  // MQTT client id (what the broker logs): empty = the bridge derives brmesh-<MAC tail>.
  s.mqttClient = s_prefs.getString("mclient", "").c_str();
  // HA hub name (dev.name): empty = the bridge falls back to the device `name`.
  // (dev.cu is always runtime-derived as http://<board IP>/ — not stored.)
  s.mqttName = s_prefs.getString("mname", "").c_str();
  // Discovery window (seconds). getInt is the non-logging path, so no seeding
  // needed; the compile-time SCAN_DURATION is the fallback for first boot.
  s.scanSec  = s_prefs.getInt("scandur", SCAN_DURATION);
  s_prefs.end();
  return s;
}

void saveSettings(const Settings &s) {
  // readOnly MUST be false — with true the put* calls below silently no-op
  // (CLAUDE.md §2).
  s_prefs.begin("brmesh", false);
  s_prefs.putString("name", s.name.c_str());
  s_prefs.putString("ssid", s.ssid.c_str());
  s_prefs.putString("pass", s.pass.c_str());
  s_prefs.putString("mhost", s.mqttHost.c_str());
  s_prefs.putInt("mport", s.mqttPort);
  s_prefs.putString("muser", s.mqttUser.c_str());
  s_prefs.putString("mpass", s.mqttPass.c_str());
  s_prefs.putString("mclient", s.mqttClient.c_str());
  s_prefs.putString("mname", s.mqttName.c_str());
  s_prefs.putInt("scandur", s.scanSec);
  s_prefs.end();
  LOGF("Saved settings to NVS");
}

void clearWiFiCreds() {
  s_prefs.begin("brmesh", false);
  s_prefs.putString("ssid", "");
  s_prefs.putString("pass", "");
  s_prefs.end();
  LOGF("Cleared saved WiFi creds from NVS");
}
