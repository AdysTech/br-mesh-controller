// ============================================================================
//  API routes — config / provision / factory-reset / raw (see ApiSystem.h).
// ============================================================================
#include "ApiSystem.h"

#include <Arduino.h>
#include <Preferences.h>
#include <ctype.h>
#include <cstring>
#include <cstdlib>

#include "MeshController.h"
#include "HaBridge.h"
#include "settings.h"
#include "WebLog.h"   // leveled logger (P2.11)
#include "ApiHttp.h"

extern MeshController g_mesh;
extern HaBridge g_bridge;   // app shell (main.cpp) — ctrl_id + live identity update

void requestProvisioning();   // app shell (main.cpp) — clears WiFi creds + reboots
void setDeviceName(const char* name);   // app shell (main.cpp)

// ----------------------------------------------------------------------------
//  GET /api/config — passwords come back masked; a field of "****" on POST
//  means "unchanged".
// ----------------------------------------------------------------------------
static esp_err_t apiConfigGetHandler(httpd_req_t *req) {
  const Settings s = loadSettings();
  cJSON *o = cJSON_CreateObject();
  if (!o) { apiSendError(req, 500, "config build failed"); return ESP_FAIL; }
  cJSON_AddStringToObject(o, "name", s.name.c_str());
  cJSON_AddStringToObject(o, "ssid", s.ssid.c_str());
  cJSON_AddStringToObject(o, "pass", s.pass.empty() ? "" : "****");
  cJSON_AddStringToObject(o, "mhost", s.mqttHost.c_str());
  cJSON_AddNumberToObject(o, "mport", (int)s.mqttPort);
  cJSON_AddStringToObject(o, "muser", s.mqttUser.c_str());
  cJSON_AddStringToObject(o, "mpass", s.mqttPass.empty() ? "" : "****");
  cJSON_AddStringToObject(o, "mclient", s.mqttClient.c_str());
  cJSON_AddStringToObject(o, "mqtt_client", g_bridge.clientId().c_str());
  cJSON_AddStringToObject(o, "mname", s.mqttName.c_str());
  cJSON_AddNumberToObject(o, "scandur", (int)s.scanSec);
  cJSON_AddStringToObject(o, "ctrl_id", g_bridge.ctrlId().c_str());
  apiSendCJson(req, 200, o);
  return ESP_OK;
}

// Name rule mirrors the console: 1-15 chars, letters/digits/'-'.
static bool validName(const std::string &n) {
  if (n.size() < 1 || n.size() > 15) return false;
  for (char c : n)
    if (!isalnum((unsigned char)c) && c != '-') return false;
  return true;
}

// ----------------------------------------------------------------------------
//  POST /api/config — save whatever fields are present.
//    name    live-applied (mDNS/hostname) — no reboot
//    ssid/pass  saved; apply by reboot (or 'provision' re-entry)
//    mhost/mport/muser/mpass  saved; used by the Phase-3 HA bridge
// ----------------------------------------------------------------------------
static esp_err_t apiConfigPostHandler(httpd_req_t *req) {
  String body = apiReadBody(req, 1024);
  Settings s = loadSettings();
  bool wifiTouched = false;
  bool mqttTouched = false;

  {
    std::string v = apiJsonString(body, "name");
    if (!v.empty()) {
      if (!validName(v)) { apiSendError(req, 400, "name: 1-15 chars, letters/digits/'-' only"); return ESP_FAIL; }
      s.name = v;
    }
  }
  // Convention for the masked fields (pass/muser/mpass): the GET response
  // returns "****" when a value is stored; POSTing "****" back means
  // UNCHANGED. A real "" clears the field (open network, no broker auth).
  {
    std::string v = apiJsonString(body, "ssid");
    // The SPA round-trips the whole cfg on every save, so the current ssid
    // echoes back even on a pure MQTT save — flag a reboot only when it CHANGED.
    if (!v.empty() && v != s.ssid) { s.ssid = v; wifiTouched = true; }
  }
  {
    std::string v = apiJsonString(body, "pass");
    if (v != "****") { s.pass = v; wifiTouched = true; }
  }
  {
    std::string v = apiJsonString(body, "mhost");
    if (!v.empty()) { s.mqttHost = v; mqttTouched = true; }
  }
  {
    int p = apiJsonInt(body, "mport");
    if (p >= 1 && p <= 65535) { s.mqttPort = (uint16_t)p; mqttTouched = true; }
  }
  {
    std::string v = apiJsonString(body, "muser");
    if (v != "****") { s.mqttUser = v; mqttTouched = true; }
  }
  {
    std::string v = apiJsonString(body, "mpass");
    if (v != "****") { s.mqttPass = v; mqttTouched = true; }
  }
  // mclient: the MQTT client id (broker-side name). Presence in the body is what
  // matters so the UI can clear it (send "") back to the derived default.
  {
    if (body.indexOf("\"mclient\"") != (unsigned)-1) {
      s.mqttClient = apiJsonString(body, "mclient");
      mqttTouched = true;
    }
  }
  // mname: empty = the bridge falls back to the device `name` as dev.name.
  // Presence in the body is what matters, so the UI can clear it by sending "".
  // (dev.cu is never stored — always http://<board IP>/ at publish time.)
  {
    if (body.indexOf("\"mname\"") != (unsigned)-1) {
      s.mqttName = apiJsonString(body, "mname");
      mqttTouched = true;
    }
  }
  // scandur: discovery window (seconds). No reboot — read live on each scan.
  // Absent (apiJsonInt -> -1) means "leave it". Clamp to a sane range.
  {
    int sd = apiJsonInt(body, "scandur");
    if (sd >= 1 && sd <= 600) s.scanSec = sd;
  }

  saveSettings(s);
  if (mqttTouched) g_bridge.updateSettings(s);
  if (apiJsonString(body, "name") == s.name)
    setDeviceName(s.name.c_str());   // persist + apply to the live netif

  // A broker change (host/port/user/pass/client/name) also needs a reboot:
  // esp-mqtt bakes the broker address+creds into the client at init, so it only
  // takes effect from the next boot. Flag that too, not just a WiFi change.
  const bool rebootNeeded = wifiTouched || mqttTouched;
  cJSON *o = cJSON_CreateObject();
  if (!o) { apiSendError(req, 500, "config build failed"); return ESP_FAIL; }
  cJSON_AddBoolToObject(o, "saved", true);
  cJSON_AddBoolToObject(o, "reboot_needed", rebootNeeded);
  cJSON_AddStringToObject(o, "name", s.name.c_str());
  apiSendCJson(req, 200, o);
  return ESP_OK;
}

// ----------------------------------------------------------------------------
//  POST /api/provision — clear saved WiFi creds, reboot into Improv.
//  The response must be on the wire before the reboot, so the app-shell call
//  happens after the send (it ends in ESP.restart()).
// ----------------------------------------------------------------------------
static esp_err_t apiProvisionHandler(httpd_req_t *req) {
  cJSON *o = cJSON_CreateObject();
  cJSON_AddBoolToObject(o, "rebooting", true);
  cJSON_AddStringToObject(o, "into", "improv");
  apiSendCJson(req, 200, o);
  vTaskDelay(pdMS_TO_TICKS(150));   // let the response flush
  requestProvisioning();            // no return
  return ESP_OK;
}

// ----------------------------------------------------------------------------
//  POST /api/factory-reset — wipe the mesh key + light list and reboot; every
//  bulb returns to factory key and must be re-keyed. Device settings (name,
//  WiFi creds, MQTT) are KEPT so the board stays reachable.
//  The mesh data and the settings share the 'brmesh' NVS namespace, so wipe
//  the whole thing and re-write just the settings keys — leaving only the
//  mesh data removed (do NOT let this drop the WiFi creds: see the bug this
//  fix closes).
// ----------------------------------------------------------------------------
static esp_err_t apiFactoryResetHandler(httpd_req_t *req) {
  cJSON *o = cJSON_CreateObject();
  cJSON_AddBoolToObject(o, "resetting", true);
  apiSendCJson(req, 200, o);
  vTaskDelay(pdMS_TO_TICKS(150));

  const Settings kept = loadSettings();   // capture creds + name + MQTT first
  Preferences p;
  p.begin("brmesh", false);
  p.clear();                              // wipes mesh key + light list + settings
  p.end();
  saveSettings(kept);                     // re-write ONLY the settings keys
  LOGF("factory-reset: wiped mesh key + light list, kept device settings, rebooting");
  delay(100);
  ESP.restart();
  return ESP_OK;
}

// ----------------------------------------------------------------------------
//  Backup / restore — a portable snapshot of the whole device state: settings
//  (name + WiFi + MQTT), the RAW 4-byte mesh key, and every registered light
//  (number, HA id, name, MAC, type, believed state). The format is flat
//  (prefix-keyed) so the board's tiny apiJson* readers can parse it with no
//  JSON lib — see the schema comment on apiBackupHandler. The mesh key rides
//  in the clear here on purpose: it is broadcast over BLE to the bulbs, so a
//  backup file is no more secret than the air itself (BRIDGE_PLAN §6).
//
//  GET  /api/backup  -> the JSON snapshot (download)
//  POST /api/restore  <- that JSON; overwrites settings + key + lights, reboots
//
//  GET /api/backup — the flat snapshot. Schema (all top-level keys flat, so
//  the restore-side apiJson* readers find them unambiguously):
//    v:1  fw  board  key:<8 hex>  ln:<n>
//    s_name s_ssid s_pass s_mhost s_mport s_muser s_mpass s_mclient s_mname
//    s_scandur
//    per light i: l<i>_num l<i>_id l<i>_nm l<i>_mac l<i>_type l<i>_on
//                 l<i>_bri l<i>_r l<i>_g l<i>_b l<i>_ct l<i>_cm
//  Built as a flat cJSON object (same shape) — cJSON owns the escaping; the
//  restore side still reads it back with the tiny apiJson* readers.
// ----------------------------------------------------------------------------
static esp_err_t apiBackupHandler(httpd_req_t *req) {
  if (!g_mesh.started()) { apiSendError(req, 503, "engine offline (provisioning mode)"); return ESP_FAIL; }
  const Settings s = loadSettings();
  const uint8_t *k = g_mesh.key();
  const std::vector<LightDevice> &lights = g_mesh.lights();

  cJSON *o = cJSON_CreateObject();
  if (!o) { apiSendError(req, 500, "backup build failed"); return ESP_FAIL; }
  cJSON_AddNumberToObject(o, "v", 1);
  cJSON_AddStringToObject(o, "fw", FIRMWARE_VERSION);
  cJSON_AddStringToObject(o, "board", kBoardModel);
  char keyHex[16];
  snprintf(keyHex, sizeof(keyHex), "%02x%02x%02x%02x", k[0], k[1], k[2], k[3]);
  cJSON_AddStringToObject(o, "key", keyHex);
  cJSON_AddNumberToObject(o, "ln", (int)lights.size());

  // settings (empty values serialize as "" — a restore of a fresh board's
  // backup is a no-op for any unset field).
  cJSON_AddStringToObject(o, "s_name", s.name.c_str());
  cJSON_AddStringToObject(o, "s_ssid", s.ssid.c_str());
  cJSON_AddStringToObject(o, "s_pass", s.pass.c_str());
  cJSON_AddStringToObject(o, "s_mhost", s.mqttHost.c_str());
  cJSON_AddNumberToObject(o, "s_mport", (int)s.mqttPort);
  cJSON_AddStringToObject(o, "s_muser", s.mqttUser.c_str());
  cJSON_AddStringToObject(o, "s_mpass", s.mqttPass.c_str());
  cJSON_AddStringToObject(o, "s_mclient", s.mqttClient.c_str());
  cJSON_AddStringToObject(o, "s_mname", s.mqttName.c_str());
  cJSON_AddNumberToObject(o, "s_scandur", (int)s.scanSec);

  // per light: flat "l<i>_<field>" keys (the restore side reads them back).
  for (int i = 0; i < (int)lights.size(); i++) {
    const LightDevice &l = lights[i];
    char key[8];
    snprintf(key, sizeof(key), "l%d_", i);
    cJSON_AddNumberToObject(o, (String(key) + "num").c_str(), (int)l.number);
    cJSON_AddStringToObject(o, (String(key) + "id").c_str(), l.id.c_str());
    cJSON_AddStringToObject(o, (String(key) + "nm").c_str(), l.name.c_str());
    cJSON_AddStringToObject(o, (String(key) + "mac").c_str(), l.address.c_str());
    char type[8];
    snprintf(type, sizeof(type), "%02x%02x", l.type[0], l.type[1]);
    cJSON_AddStringToObject(o, (String(key) + "type").c_str(), type);
    cJSON_AddNumberToObject(o, (String(key) + "on").c_str(), l.on ? 1 : 0);
    cJSON_AddNumberToObject(o, (String(key) + "bri").c_str(), (int)l.brightness);
    cJSON_AddNumberToObject(o, (String(key) + "r").c_str(), (int)l.rgbR);
    cJSON_AddNumberToObject(o, (String(key) + "g").c_str(), (int)l.rgbG);
    cJSON_AddNumberToObject(o, (String(key) + "b").c_str(), (int)l.rgbB);
    cJSON_AddNumberToObject(o, (String(key) + "ct").c_str(), (int)l.ct);
    cJSON_AddNumberToObject(o, (String(key) + "cm").c_str(), (int)l.colorMode);
  }
  apiSendCJson(req, 200, o);
  return ESP_OK;
}

// Validate an 8-char hex string into a 4-byte key. Returns false on any bad
// length / non-hex / out-of-range byte.
static bool parseBackupKey(const std::string &hex, uint8_t key[4]) {
  if (hex.size() != 8) return false;
  for (int i = 0; i < 4; i++) {
    const std::string byte = hex.substr(i * 2, 2);
    char *end = nullptr;
    unsigned long v = strtoul(byte.c_str(), &end, 16);
    if (end != byte.c_str() + 2 || v > 0xFF) return false;
    key[i] = (uint8_t)v;
  }
  return true;
}

// ----------------------------------------------------------------------------
//  POST /api/restore — apply a backup (settings + key + lights), then reboot so
//  begin() re-keys nothing and loadLights() re-fires onRegistered (HA re-
//  publish). The response goes out BEFORE the write + reboot (same contract as
//  factory-reset). Note: if the backup's WiFi differs, the board comes back on
//  that network, not this one.
// ----------------------------------------------------------------------------
static esp_err_t apiRestoreHandler(httpd_req_t *req) {
  if (!g_mesh.started()) { apiSendError(req, 503, "engine offline (provisioning mode)"); return ESP_FAIL; }

  String body = apiReadBody(req, 32768);

  uint8_t key[4];
  if (!parseBackupKey(apiJsonString(body, "key"), key)) {
    apiSendError(req, 400, "backup malformed: 'key' must be 8 hex chars");
    return ESP_FAIL;
  }
  int ln = apiJsonInt(body, "ln");
  if (ln < 0) ln = 0;
  if (ln > 64) { apiSendError(req, 400, "backup malformed: too many lights (max 64)"); return ESP_FAIL; }

  Settings s;
  s.name       = apiJsonString(body, "s_name");
  s.ssid       = apiJsonString(body, "s_ssid");
  s.pass       = apiJsonString(body, "s_pass");
  s.mqttHost   = apiJsonString(body, "s_mhost");
  s.mqttPort   = (uint16_t)apiJsonInt(body, "s_mport");
  if (s.mqttPort < 1) s.mqttPort = 1883;
  s.mqttUser   = apiJsonString(body, "s_muser");
  s.mqttPass   = apiJsonString(body, "s_mpass");
  s.mqttClient = apiJsonString(body, "s_mclient");
  s.mqttName   = apiJsonString(body, "s_mname");
  s.scanSec    = apiJsonInt(body, "s_scandur");
  if (s.scanSec < 1 || s.scanSec > 600) s.scanSec = SCAN_DURATION;

  std::vector<LightDevice> lights;
  lights.reserve(ln);
  for (int i = 0; i < ln; i++) {
    char key[8];
    snprintf(key, sizeof(key), "l%d_", i);
    LightDevice l;
    l.number     = (uint8_t)apiJsonInt(body, (String(key) + "num").c_str());
    l.id         = apiJsonString(body, (String(key) + "id").c_str());
    l.name       = apiJsonString(body, (String(key) + "nm").c_str());
    l.address    = apiJsonString(body, (String(key) + "mac").c_str());
    std::string t = apiJsonString(body, (String(key) + "type").c_str());
    if (t.size() >= 4) {
      l.type[0] = (uint8_t)strtoul(t.substr(0, 2).c_str(), nullptr, 16);
      l.type[1] = (uint8_t)strtoul(t.substr(2, 2).c_str(), nullptr, 16);
    }
    l.on         = apiJsonInt(body, (String(key) + "on").c_str()) != 0;
    l.brightness = (uint8_t)apiJsonInt(body, (String(key) + "bri").c_str());
    l.rgbR       = (uint8_t)apiJsonInt(body, (String(key) + "r").c_str());
    l.rgbG       = (uint8_t)apiJsonInt(body, (String(key) + "g").c_str());
    l.rgbB       = (uint8_t)apiJsonInt(body, (String(key) + "b").c_str());
    l.ct         = (uint16_t)apiJsonInt(body, (String(key) + "ct").c_str());
    l.colorMode  = (uint8_t)apiJsonInt(body, (String(key) + "cm").c_str());
    l.isRegistered = true;
    lights.push_back(l);
  }

  cJSON *o = cJSON_CreateObject();
  if (!o) { apiSendError(req, 500, "restore build failed"); return ESP_FAIL; }
  cJSON_AddBoolToObject(o, "restored", true);
  cJSON_AddNumberToObject(o, "lights", ln);
  apiSendCJson(req, 200, o);
  vTaskDelay(pdMS_TO_TICKS(150));   // let the response flush
  saveSettings(s);
  g_mesh.restoreKey(key);
  g_mesh.restoreLights(lights);
  LOGF("restore: applied settings + key + %d light(s), rebooting", ln);
  delay(100);
  ESP.restart();
  return ESP_OK;
}

// ----------------------------------------------------------------------------
//  POST /api/raw  {"sec":N, "mode":"active"|"passive"|"wake"} — raw FastCon
//  capture into the log ring (the field path for a new bulb model, BRIDGE_PLAN
//  §10). mode (default "active"):
//    active  — normal active scan (SCAN_REQ), every frame, unfiltered
//    passive — pure listening, no SCAN_REQ (best for a phone one-shot — active's
//              TX can steal the RX time a one-shot needs)
//    wake    — WAKE broadcast + time-sliced raw capture
//  Runs in the background; end early w/ /api/scan/stop.
// ----------------------------------------------------------------------------
static esp_err_t apiRawPostHandler(httpd_req_t *req) {
  if (!g_mesh.started()) { apiSendError(req, 503, "engine offline (provisioning mode)"); return ESP_FAIL; }
  if (g_mesh.isScanning()) { apiSendError(req, 409, "a scan is already running"); return ESP_FAIL; }

  String body = apiReadBody(req, 256);
  int sec = apiJsonInt(body, "sec");
  if (sec < 1) sec = 5;
  if (sec > 120) sec = 120;
  std::string mode = apiJsonString(body, "mode");
  if (mode.empty()) mode = "active";

  if (mode == "wake") g_mesh.rawWake(sec);
  else if (mode == "passive") g_mesh.rawPassive(sec);
  else g_mesh.raw(sec);

  cJSON *o = cJSON_CreateObject();
  if (!o) { apiSendError(req, 500, "raw build failed"); return ESP_FAIL; }
  cJSON_AddStringToObject(o, "capture", mode.c_str());
  cJSON_AddNumberToObject(o, "sec", sec);
  apiSendCJson(req, 202, o);
  return ESP_OK;
}

// ----------------------------------------------------------------------------
bool registerSystemRoutes(httpd_handle_t srv) {
  httpd_uri_t u;

  memset(&u, 0, sizeof(u));
  u.uri = "/api/config"; u.method = HTTP_GET; u.handler = apiConfigGetHandler;
  if (httpd_register_uri_handler(srv, &u) != ESP_OK) return false;

  memset(&u, 0, sizeof(u));
  u.uri = "/api/config"; u.method = HTTP_POST; u.handler = apiConfigPostHandler;
  if (httpd_register_uri_handler(srv, &u) != ESP_OK) return false;

  memset(&u, 0, sizeof(u));
  u.uri = "/api/provision"; u.method = HTTP_POST; u.handler = apiProvisionHandler;
  if (httpd_register_uri_handler(srv, &u) != ESP_OK) return false;

  memset(&u, 0, sizeof(u));
  u.uri = "/api/factory-reset"; u.method = HTTP_POST; u.handler = apiFactoryResetHandler;
  if (httpd_register_uri_handler(srv, &u) != ESP_OK) return false;

  memset(&u, 0, sizeof(u));
  u.uri = "/api/backup"; u.method = HTTP_GET; u.handler = apiBackupHandler;
  if (httpd_register_uri_handler(srv, &u) != ESP_OK) return false;

  memset(&u, 0, sizeof(u));
  u.uri = "/api/restore"; u.method = HTTP_POST; u.handler = apiRestoreHandler;
  if (httpd_register_uri_handler(srv, &u) != ESP_OK) return false;

  memset(&u, 0, sizeof(u));
  u.uri = "/api/raw"; u.method = HTTP_POST; u.handler = apiRawPostHandler;
  if (httpd_register_uri_handler(srv, &u) != ESP_OK) return false;

  LOGD("web: + /api/config, /api/provision, /api/factory-reset, /api/backup, /api/restore, /api/raw");
  return true;
}
