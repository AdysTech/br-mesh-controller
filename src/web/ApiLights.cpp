// ============================================================================
//  API routes — scan + per-light control (see ApiLights.h).
//
//  All control goes through the MeshController public surface (the one seam —
//  BRIDGE_PLAN §7/§8): this file never touches BLE or the codec.
// ============================================================================
#include "ApiLights.h"

#include <Arduino.h>
#include <cstring>
#include <cstdlib>
#include "WebLog.h"   // leveled logger (P2.11)

#include "config.h"
#include "MeshController.h"
#include "ApiHttp.h"

extern MeshController g_mesh;

// ----------------------------------------------------------------------------
//  POST /api/scan  {"seconds":N} — request discovery + key new bulbs.
//  requestScan() queues it; loop() runs it (one scan at a time, §9).
// ----------------------------------------------------------------------------
static esp_err_t apiScanPostHandler(httpd_req_t *req) {
  if (!g_mesh.started()) { apiSendError(req, 503, "engine offline (provisioning mode)"); return ESP_FAIL; }
  if (g_mesh.isScanning()) { apiSendError(req, 409, "a scan is already running"); return ESP_FAIL; }

  int seconds = SCAN_DURATION;
  int v = apiJsonInt(apiReadBody(req, 256), "seconds");
  if (v > 0) seconds = (v > 120) ? 120 : v;   // sanity cap

  g_mesh.requestScan(seconds);
  char buf[96];
  int n = snprintf(buf, sizeof(buf), "{\"queued\":true,\"seconds\":%d}", seconds);
  apiSendJson(req, 202, buf);
  return ESP_OK;
}

// POST /api/scan/stop — end a running scan/capture early (console 'scanstop').
static esp_err_t apiScanStopHandler(httpd_req_t *req) {
  g_mesh.scanStop();
  apiSendJson(req, 200, "{\"stopped\":true}");
  return ESP_OK;
}

// ----------------------------------------------------------------------------
//  Light helpers
// ----------------------------------------------------------------------------
static bool lightExists(uint8_t number) {
  for (const auto &l : g_mesh.lights())
    if (l.number == number) return true;
  return false;
}

/// {rgb:[r,g,b]} — pull the three ints between the first '[' after "rgb" and
/// its closing ']'. Returns false when the field is absent or malformed.
static bool parseRgb(const String &body, uint8_t &r, uint8_t &g, uint8_t &b) {
  size_t p = body.indexOf("\"rgb\"");
  if (p == -1) return false;
  p = body.indexOf('[', p);
  if (p == -1) return false;
  p++;

  int vals[3] = { 0, 0, 0 };
  int i = 0;
  int cur = 0;
  bool any = false;
  for (size_t len = body.length(); p < len; p++) {
    char c = body[p];
    if (c >= '0' && c <= '9') { cur = cur * 10 + (c - '0'); any = true; }
    else if (c == ',' && i < 2) { if (!any) return false; vals[i++] = cur; cur = 0; any = false; }
    else if (c == ']') { if (!any || i != 2) return false; vals[2] = cur; break; }
    else if (!(c == ' ' || c == '\t')) return false;
  }
  if (i != 2) return false;
  for (int k = 0; k < 3; k++) if (vals[k] < 0 || vals[k] > 255) return false;
  r = (uint8_t)vals[0]; g = (uint8_t)vals[1]; b = (uint8_t)vals[2];
  return true;
}

// ----------------------------------------------------------------------------
//  POST /api/light?n=N — one control op per body:
//    {"on":true|false} | {"brightness":0-127} | {"rgb":[r,g,b]} | {"ct":2700-6000 kelvin warm→cool}
// ----------------------------------------------------------------------------
static esp_err_t apiLightPostHandler(httpd_req_t *req) {
  if (!g_mesh.started()) { apiSendError(req, 503, "engine offline (provisioning mode)"); return ESP_FAIL; }

  int num = atoi(apiQuery(req, "n").c_str());
  if (num < 1) { apiSendError(req, 400, "missing light number — /api/light?n=<n>"); return ESP_FAIL; }
  if (!lightExists((uint8_t)num)) { apiSendError(req, 404, "no such light (see /api/lights)"); return ESP_FAIL; }
  if (g_mesh.isScanning()) { apiSendError(req, 409, "a scan is running — wait for it to finish"); return ESP_FAIL; }

  String body = apiReadBody(req, 256);
  uint8_t r, g, b;
  int brightness = apiJsonInt(body, "brightness");
  int ct = apiJsonInt(body, "ct");

  if (body.indexOf("\"on\"") != -1) {
    g_mesh.state((uint8_t)num, apiJsonBool(body, "on"));
  } else if (brightness >= 0) {
    g_mesh.brightness((uint8_t)num, (uint8_t)(brightness > 127 ? 127 : brightness));
  } else if (parseRgb(body, r, g, b)) {
    g_mesh.rgb((uint8_t)num, r, g, b);
  } else if (ct >= 0) {
    g_mesh.colorTemp((uint8_t)num, (uint16_t)ct);
  } else {
    apiSendError(req, 400, "no op recognized — use {on}/{brightness}/{rgb}/{ct}");
    return ESP_FAIL;
  }

  char buf[96];
  int n = snprintf(buf, sizeof(buf), "{\"sent\":true,\"n\":%d}", num);
  apiSendJson(req, 200, buf);
  return ESP_OK;
}

// ----------------------------------------------------------------------------
//  PATCH /api/light?n=N  {"name":"..."} — rename (FR-13)
// ----------------------------------------------------------------------------
static esp_err_t apiLightPatchHandler(httpd_req_t *req) {
  int num = atoi(apiQuery(req, "n").c_str());
  if (num < 1) { apiSendError(req, 400, "missing light number — /api/light?n=<n>"); return ESP_FAIL; }

  std::string name = apiJsonString(apiReadBody(req, 256), "name");
  if (name.empty()) { apiSendError(req, 400, "body must be {\"name\":\"...\"}"); return ESP_FAIL; }
  if (name.size() > 32) name.resize(32);

  if (!g_mesh.rename((uint8_t)num, name)) {
    apiSendError(req, 404, "no such light (see /api/lights)");
    return ESP_FAIL;
  }
  char buf[128];
  int n = snprintf(buf, sizeof(buf), "{\"renamed\":%d,\"name\":\"%s\"}", num,
                   jsonEscape(name.c_str()).c_str());
  apiSendJson(req, 200, buf);
  return ESP_OK;
}

// ----------------------------------------------------------------------------
//  DELETE /api/light?n=N — remove from list + NVS (FR-12).
//  The bulb keeps its key (nothing is sent over the air); re-addable by
//  rebooting the bulb + `scan`.
// ----------------------------------------------------------------------------
static esp_err_t apiLightDeleteHandler(httpd_req_t *req) {
  int num = atoi(apiQuery(req, "n").c_str());
  if (num < 1) { apiSendError(req, 400, "missing light number — /api/light?n=<n>"); return ESP_FAIL; }
  if (!lightExists((uint8_t)num)) { apiSendError(req, 404, "no such light (see /api/lights)"); return ESP_FAIL; }

  g_mesh.forget(std::to_string(num));
  char buf[64];
  int n = snprintf(buf, sizeof(buf), "{\"deleted\":%d}", num);
  apiSendJson(req, 200, buf);
  return ESP_OK;
}

// ----------------------------------------------------------------------------
bool registerLightRoutes(httpd_handle_t srv) {
  httpd_uri_t u;
  memset(&u, 0, sizeof(u));
  u.uri = "/api/scan"; u.method = HTTP_POST; u.handler = apiScanPostHandler;
  if (httpd_register_uri_handler(srv, &u) != ESP_OK) return false;

  memset(&u, 0, sizeof(u));
  u.uri = "/api/scan/stop"; u.method = HTTP_POST; u.handler = apiScanStopHandler;
  if (httpd_register_uri_handler(srv, &u) != ESP_OK) return false;

  memset(&u, 0, sizeof(u));
  u.uri = "/api/light"; u.method = HTTP_POST; u.handler = apiLightPostHandler;
  if (httpd_register_uri_handler(srv, &u) != ESP_OK) return false;

  memset(&u, 0, sizeof(u));
  u.uri = "/api/light"; u.method = HTTP_PATCH; u.handler = apiLightPatchHandler;
  if (httpd_register_uri_handler(srv, &u) != ESP_OK) return false;

  memset(&u, 0, sizeof(u));
  u.uri = "/api/light"; u.method = HTTP_DELETE; u.handler = apiLightDeleteHandler;
  if (httpd_register_uri_handler(srv, &u) != ESP_OK) return false;

  LOGD("web: + /api/scan, /api/scan/stop, /api/light (POST/PATCH/DELETE ?n=)");
  return true;
}
