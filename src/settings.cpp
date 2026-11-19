// NVS-backed runtime settings (see settings.h).
// Lives in the same "brmesh" namespace as the mesh key + light list.
#include "settings.h"
#include <Arduino.h>
#include <Preferences.h>
#include "config.h"

static Preferences s_prefs;

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
  s_prefs.end();
  return s;
}

void saveSettings(const Settings &s) {
  // readOnly MUST be false — with true the put* calls below silently no-op
  // (proven gotcha, CLAUDE.md §2).
  s_prefs.begin("brmesh", false);
  s_prefs.putString("name", s.name.c_str());
  s_prefs.putString("ssid", s.ssid.c_str());
  s_prefs.putString("pass", s.pass.c_str());
  s_prefs.putString("mhost", s.mqttHost.c_str());
  s_prefs.putInt("mport", s.mqttPort);
  s_prefs.putString("muser", s.mqttUser.c_str());
  s_prefs.putString("mpass", s.mqttPass.c_str());
  s_prefs.end();
  Serial.println("Saved settings to NVS");
}

void clearWiFiCreds() {
  s_prefs.begin("brmesh", false);
  s_prefs.putString("ssid", "");
  s_prefs.putString("pass", "");
  s_prefs.end();
  Serial.println("Cleared saved WiFi creds from NVS");
}
