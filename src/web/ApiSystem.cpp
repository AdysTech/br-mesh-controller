// ============================================================================
//  API routes — config / provision / factory-reset / raw (see ApiSystem.h).
// ============================================================================
#include "ApiSystem.h"

#include <Arduino.h>
#include <Preferences.h>
#include <ctype.h>
#include <cstring>

#include "MeshController.h"
#include "settings.h"
#include "WebLog.h"   // leveled logger (P2.11)
#include "ApiHttp.h"

extern MeshController g_mesh;

void requestProvisioning();   // app shell (main.cpp) — clears WiFi creds + reboots
void setDeviceName(const char* name);   // app shell (main.cpp)

// ----------------------------------------------------------------------------
//  GET /api/config — passwords come back masked; a field of "****" on POST
//  means "unchanged".
// ----------------------------------------------------------------------------
static esp_err_t apiConfigGetHandler(httpd_req_t *req) {
  const Settings s = loadSettings();
  char buf[512];
  int n = snprintf(buf, sizeof(buf),
      "{\"name\":\"%s\","
      "\"ssid\":\"%s\","
      "\"pass\":\"%s\","
      "\"mhost\":\"%s\","
      "\"mport\":%u,"
      "\"muser\":\"%s\","
      "\"mpass\":\"%s\"}",
      jsonEscape(s.name.c_str()).c_str(),
      jsonEscape(s.ssid.c_str()).c_str(),
      s.pass.empty() ? "" : "****",
      jsonEscape(s.mqttHost.c_str()).c_str(),
      (unsigned)s.mqttPort,
      jsonEscape(s.mqttUser.c_str()).c_str(),
      s.mqttPass.empty() ? "" : "****");
  if (n <= 0) { apiSendError(req, 500, "config build failed"); return ESP_FAIL; }
  apiSendJson(req, 200, buf);
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
    if (!v.empty()) { s.ssid = v; wifiTouched = true; }
  }
  {
    std::string v = apiJsonString(body, "pass");
    if (v != "****") { s.pass = v; wifiTouched = true; }
  }
  {
    std::string v = apiJsonString(body, "mhost");
    if (!v.empty()) s.mqttHost = v;
  }
  {
    int p = apiJsonInt(body, "mport");
    if (p >= 1 && p <= 65535) s.mqttPort = (uint16_t)p;
  }
  {
    std::string v = apiJsonString(body, "muser");
    if (v != "****") s.mqttUser = v;
  }
  {
    std::string v = apiJsonString(body, "mpass");
    if (v != "****") s.mqttPass = v;
  }

  saveSettings(s);
  if (apiJsonString(body, "name") == s.name)
    setDeviceName(s.name.c_str());   // persist + apply to the live netif

  char buf[160];
  int n = snprintf(buf, sizeof(buf),
                   "{\"saved\":true,\"reboot_needed\":%s,\"name\":\"%s\"}",
                   wifiTouched ? "true" : "false",
                   jsonEscape(s.name.c_str()).c_str());
  apiSendJson(req, 200, buf);
  return ESP_OK;
}

// ----------------------------------------------------------------------------
//  POST /api/provision — clear saved WiFi creds, reboot into Improv.
//  The response must be on the wire before the reboot, so the app-shell call
//  happens after the send (it ends in ESP.restart()).
// ----------------------------------------------------------------------------
static esp_err_t apiProvisionHandler(httpd_req_t *req) {
  apiSendJson(req, 200, "{\"rebooting\":true,\"into\":\"improv\"}");
  vTaskDelay(pdMS_TO_TICKS(150));   // let the response flush
  requestProvisioning();            // no return
  return ESP_OK;
}

// ----------------------------------------------------------------------------
//  POST /api/factory-reset — wipe the whole brmesh NVS namespace (mesh key +
//  light list) and reboot; every bulb returns to factory key and must be
//  re-keyed. WiFi creds are KEPT (the board stays reachable).
// ----------------------------------------------------------------------------
static esp_err_t apiFactoryResetHandler(httpd_req_t *req) {
  apiSendJson(req, 200, "{\"resetting\":true}");
  vTaskDelay(pdMS_TO_TICKS(150));

  LOGF("factory-reset: wiping NVS 'brmesh' (mesh key + light list), rebooting");
  Preferences p;
  p.begin("brmesh", false);
  p.clear();
  p.end();
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

  char buf[96];
  int n = snprintf(buf, sizeof(buf), "{\"capture\":\"%s\",\"sec\":%d}", mode.c_str(), sec);
  apiSendJson(req, 202, buf);
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
  u.uri = "/api/raw"; u.method = HTTP_POST; u.handler = apiRawPostHandler;
  if (httpd_register_uri_handler(srv, &u) != ESP_OK) return false;

  LOGD("web: + /api/config, /api/provision, /api/factory-reset, /api/raw");
  return true;
}
