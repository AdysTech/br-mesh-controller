// ============================================================================
//  WebServer (see WebServer.h) — esp_http_server startup, routes, SPA, /ws/log.
//
//  The httpd config + WS dispatch model mirror lora-relay's build on this
//  core (frame-struct WS API, client id = httpd_req_to_sockfd):
//    - esp_http_server itself does the 101 handshake.
//    - The WS handler is invoked when the CLIENT SENDS A FRAME — not at
//      handshake. So the client must send a hello frame, the handler consumes
//      it (2-phase httpd_ws_recv_frame), greets a NEW client synchronously
//      (httpd_ws_send_frame), and is idempotent for repeat frames.
//    - handle_ws_control_frames = false: the server answers PING/PONG/CLOSE
//      itself; they never reach the handler.
// ============================================================================
#include "WebServer.h"

#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <esp_http_server.h>
#include <esp_heap_caps.h>   // min-seen heap + largest free internal block
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <cstring>
#include <string>

#include "config.h"
#include "settings.h"
#include "MeshController.h"
#include "HaBridge.h"
#include "ApiHttp.h"
#include "ApiLights.h"
#include "ApiSystem.h"
#include "WebLog.h"

extern MeshController g_mesh;   // app shell (main.cpp)
extern HaBridge g_bridge;       // app shell (main.cpp)

httpd_handle_t g_webServer = nullptr;

// The SPA build tag this board is SERVING (data/spa.version, written by
// tools/build_spa.py; the same FIRMWARE_VERSION the firmware was built from).
static char s_spaVersion[32] = "";
const char *webSpaVersion() { return s_spaVersion; }

static void loadSpaVersion() {
  s_spaVersion[0] = 0;
  if (!LittleFS.exists("/spa.version")) return;
  File f = LittleFS.open("/spa.version", "r");
  if (!f) return;
  size_t n = f.read((uint8_t *)s_spaVersion, sizeof(s_spaVersion) - 1);
  f.close();
  if (n > 0) {
    s_spaVersion[n] = 0;
    char *nl = strchr(s_spaVersion, '\n');
    if (nl) *nl = 0;
    LOGF("web: serving SPA build %s", s_spaVersion);
  }
}

// ----------------------------------------------------------------------------
//  WS client registry — socket fds, lazily pruned when a send to one fails.
//  (Same shape as lora-relay's WebSocketPush.cpp.)
// ----------------------------------------------------------------------------
static int      s_wsFds[WEB_MAX_WS_CLIENTS] = {};
static int      s_wsCount = 0;
static SemaphoreHandle_t s_wsMux = nullptr;

static void wsEnsureMux(void) {
  if (!s_wsMux) s_wsMux = xSemaphoreCreateMutex();
}

static void wsAddClient(int fd) {
  wsEnsureMux();
  xSemaphoreTake(s_wsMux, portMAX_DELAY);
  if (s_wsCount < WEB_MAX_WS_CLIENTS) s_wsFds[s_wsCount++] = fd;
  xSemaphoreGive(s_wsMux);
}

static void wsDropClient(int fd) {
  wsEnsureMux();
  xSemaphoreTake(s_wsMux, portMAX_DELAY);
  for (int i = 0; i < s_wsCount; i++)
    if (s_wsFds[i] == fd) { s_wsFds[i] = s_wsFds[--s_wsCount]; break; }
  xSemaphoreGive(s_wsMux);
}

static bool wsHasClient(int fd) {
  wsEnsureMux();
  xSemaphoreTake(s_wsMux, portMAX_DELAY);
  bool found = false;
  for (int i = 0; i < s_wsCount; i++) found = found || (s_wsFds[i] == fd);
  xSemaphoreGive(s_wsMux);
  return found;
}

bool webWsHasLogClients(void) {
  wsEnsureMux();
  xSemaphoreTake(s_wsMux, portMAX_DELAY);
  bool any = (s_wsCount > 0);
  xSemaphoreGive(s_wsMux);
  return any;
}

// ----------------------------------------------------------------------------
//  Broadcast (the WebLog hook): {"type":"log","text":...} to every client;
//  dead fds are pruned.
// ----------------------------------------------------------------------------
void wsLogBroadcast(const char *msg) {
  if (!msg || !g_webServer) return;

  char bcast[512];
  int n = snprintf(bcast, sizeof(bcast),
                   "{\"type\":\"log\",\"text\":\"%s\"}", jsonEscape(msg).c_str());
  if (n <= 0 || n >= (int)sizeof(bcast)) return;

  int snapshot[WEB_MAX_WS_CLIENTS];
  int snapCount = 0;
  wsEnsureMux();
  xSemaphoreTake(s_wsMux, portMAX_DELAY);
  if (s_wsCount > 0) {
    snapCount = s_wsCount < WEB_MAX_WS_CLIENTS ? s_wsCount : WEB_MAX_WS_CLIENTS;
    memcpy(snapshot, s_wsFds, snapCount * sizeof(int));
  }
  xSemaphoreGive(s_wsMux);

  httpd_ws_frame_t frame;
  memset(&frame, 0, sizeof(frame));
  frame.type    = HTTPD_WS_TYPE_TEXT;
  frame.payload = (uint8_t *)bcast;
  frame.len     = (size_t)n;

  for (int i = 0; i < snapCount; i++)
    if (httpd_ws_send_frame_async(g_webServer, snapshot[i], &frame) != ESP_OK)
      wsDropClient(snapshot[i]);   // dead connection — prune it
}

// ----------------------------------------------------------------------------
//  WS /ws/log handler (dispatch model in the file header).
// ----------------------------------------------------------------------------
static esp_err_t wsLogHandler(httpd_req_t *req) {
  int fd = httpd_req_to_sockfd(req);

  // Consume the frame that triggered this invocation (client hello) so it
  // does not linger in the socket buffer. 2-phase recv, fixed stack buffer.
  char buf[256];
  httpd_ws_frame_t pkt;
  memset(&pkt, 0, sizeof(pkt));
  esp_err_t r = httpd_ws_recv_frame(req, &pkt, 0);          // phase 1: size
  if (r == ESP_OK && pkt.len > 0 && pkt.len <= sizeof(buf) - 1) {
    pkt.payload = (uint8_t *)buf;
    httpd_ws_recv_frame(req, &pkt, pkt.len + 1);            // phase 2: drain
  }

  // Re-invocation on an already-registered session — frame consumed, done.
  if (wsHasClient(fd)) return ESP_OK;

  // New connection -> greet with the recent log tail (one frame per line,
  // so a reconnect loses nothing), then join the broadcast set.
  std::string tail = logTail(50);
  size_t start = 0;
  while (start <= tail.size()) {
    size_t nl = tail.find('\n', start);
    std::string line = (nl == std::string::npos)
        ? tail.substr(start) : tail.substr(start, nl - start);
    if (!line.empty()) {
      char frame[512];
      int len = snprintf(frame, sizeof(frame),
                         "{\"type\":\"log\",\"text\":\"%s\"}",
                         jsonEscape(line.c_str()).c_str());
      httpd_ws_frame_t f;
      memset(&f, 0, sizeof(f));
      f.type    = HTTPD_WS_TYPE_TEXT;
      f.payload = (uint8_t *)frame;
      f.len     = (size_t)len;
      if (httpd_ws_send_frame(req, &f) != ESP_OK) return ESP_FAIL;
    }
    if (nl == std::string::npos) break;
    start = nl + 1;
  }

  wsAddClient(fd);
  return ESP_OK;
}

// ----------------------------------------------------------------------------
//  GET /api/status
// ----------------------------------------------------------------------------
static esp_err_t apiStatusHandler(httpd_req_t *req) {
  const Settings s = loadSettings();
  cJSON *o = cJSON_CreateObject();
  if (!o) { apiSendError(req, 500, "status build failed"); return ESP_FAIL; }
  cJSON_AddStringToObject(o, "name", s.name.c_str());
  cJSON_AddStringToObject(o, "mode", "sta");
  cJSON_AddStringToObject(o, "ip", WiFi.localIP().toString().c_str());
  cJSON_AddStringToObject(o, "ssid", s.ssid.c_str());
  cJSON_AddNumberToObject(o, "rssi", (int)WiFi.RSSI());
  cJSON_AddBoolToObject(o, "mqtt", g_bridge.connected());
  cJSON_AddBoolToObject(o, "mqtt_enabled", g_bridge.enabled());
  cJSON_AddStringToObject(o, "ctrl_id", g_bridge.ctrlId().c_str());
  cJSON_AddNumberToObject(o, "lights", (int)g_mesh.lightCount());
  cJSON_AddBoolToObject(o, "scanning", g_mesh.isScanning());
  cJSON_AddNumberToObject(o, "scandur", (int)s.scanSec);
  cJSON_AddNumberToObject(o, "queue_depth", (int)g_mesh.queueDepth());
  cJSON_AddNumberToObject(o, "queue_overflow", (double)g_mesh.queueOverflow());
  cJSON_AddStringToObject(o, "fw", FIRMWARE_VERSION);
  cJSON_AddStringToObject(o, "spa", webSpaVersion());
  cJSON_AddStringToObject(o, "board", kBoardModel);
  cJSON_AddNumberToObject(o, "uptime_s", (double)(millis() / 1000));
  cJSON_AddNumberToObject(o, "heap_free", (double)ESP.getFreeHeap());
  cJSON_AddNumberToObject(o, "free_heap_kb", (double)(esp_get_free_heap_size() / 1024));
  cJSON_AddNumberToObject(o, "min_free_heap_kb", (double)(esp_get_minimum_free_heap_size() / 1024));
  cJSON_AddNumberToObject(o, "largest_free_internal_block_kb",
      (double)((uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
  cJSON_AddNumberToObject(o, "psram_size_kb", (double)(ESP.getPsramSize() / 1024));
  cJSON_AddNumberToObject(o, "psram_free_kb", (double)(ESP.getFreePsram() / 1024));
  apiSendCJson(req, 200, o);
  return ESP_OK;
}

// ----------------------------------------------------------------------------
//  GET /api/lights
// ----------------------------------------------------------------------------
static esp_err_t apiLightsHandler(httpd_req_t *req) {
  const auto &lights = g_mesh.lights();
  cJSON *arr = cJSON_CreateArray();
  if (!arr) { apiSendError(req, 500, "lights build failed"); return ESP_FAIL; }
  for (size_t i = 0; i < lights.size(); i++) {
    const LightDevice &l = lights[i];
    // mode drives the UI picker: white (on, no colour), rgb, or ct.
    const char *mode =
        l.colorMode == 2 ? "ct" : (l.colorMode == 1 ? "rgb" : "white");
    cJSON *o = cJSON_CreateObject();
    if (!o) { cJSON_Delete(arr); apiSendError(req, 500, "lights build failed"); return ESP_FAIL; }
    cJSON_AddNumberToObject(o, "n", (int)l.number);
    cJSON_AddStringToObject(o, "name", l.name.c_str());
    cJSON_AddStringToObject(o, "id", l.id.c_str());
    cJSON_AddStringToObject(o, "mac", l.address.c_str());
    char type[8];
    snprintf(type, sizeof(type), "%02X%02X", l.type[0], l.type[1]);
    cJSON_AddStringToObject(o, "type", type);
    if (cJSON *feat = cJSON_AddArrayToObject(o, "features")) {
      cJSON_AddItemToArray(feat, cJSON_CreateString("on_off"));
      cJSON_AddItemToArray(feat, cJSON_CreateString("brightness"));
      cJSON_AddItemToArray(feat, cJSON_CreateString("rgb"));
      cJSON_AddItemToArray(feat, cJSON_CreateString("ct"));
    }
    cJSON_AddNumberToObject(o, "min_kelvin", 2700);
    cJSON_AddNumberToObject(o, "max_kelvin", 6000);
    cJSON *bs = cJSON_AddObjectToObject(o, "believed_state");
    cJSON_AddBoolToObject(bs, "on", l.on);
    cJSON_AddNumberToObject(bs, "brightness", (int)l.brightness);
    if (cJSON *rgb = cJSON_AddArrayToObject(bs, "rgb")) {
      cJSON_AddItemToArray(rgb, cJSON_CreateNumber((double)l.rgbR));
      cJSON_AddItemToArray(rgb, cJSON_CreateNumber((double)l.rgbG));
      cJSON_AddItemToArray(rgb, cJSON_CreateNumber((double)l.rgbB));
    }
    cJSON_AddNumberToObject(bs, "ct", (int)l.ct);
    cJSON_AddStringToObject(bs, "mode", mode);
    cJSON_AddItemToArray(arr, o);
  }
  apiSendCJson(req, 200, arr);
  return ESP_OK;
}

// ----------------------------------------------------------------------------
//  GET / (+ /*) — the SPA (index.html.gz in LittleFS). The SPA is now the only
//  front-end; if the slot is empty (the SPA wasn't flashed, or LittleFS failed
//  to mount) we return a short diagnostic instead of a stale embedded page.
// ----------------------------------------------------------------------------
static esp_err_t indexHandler(httpd_req_t *req) {
  // 1) LittleFS SPA: /index.html.gz. The file is gzip-compressed, so the raw
  //    bytes go out with Content-Encoding: gzip (the browser gunzips).
  if (LittleFS.exists("/index.html.gz")) {
    File f = LittleFS.open("/index.html.gz", "r");
    if (f) {
      httpd_resp_set_type(req, "text/html; charset=utf-8");
      httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
      httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
      httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
      uint8_t buf[4096];
      esp_err_t err = ESP_OK;
      for (;;) {
        size_t n = f.read(buf, sizeof(buf));
        if (n == 0) break;
        err = httpd_resp_send_chunk(req, (const char *)buf, (ssize_t)n);
        if (err != ESP_OK) break;
      }
      f.close();
      if (err != ESP_OK) return err;
      return httpd_resp_send_chunk(req, nullptr, 0);   // terminates chunking
    }
  }

  // 2) The SPA slot is empty — the front-end wasn't flashed (or LittleFS failed
  //    to mount). Return a short diagnostic so this is diagnosable, not a blank
  //    page; the REST API under /api/* is still live.
  const char *msg =
      "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>BRMesh</title></head>"
      "<body style=\"font-family:system-ui,sans-serif;background:#111;color:#ddd;"
      "margin:3rem auto;max-width:36rem;padding:0 1rem\">"
      "<h1 style=\"font-size:1.2rem\">SPA not found</h1>"
      "<p>LittleFS has no <code>index.html.gz</code> — the front-end wasn't flashed. "
      "Build + flash it with:</p>"
      "<pre style=\"background:#000;padding:.75rem;overflow:auto\">pio run --target uploadfs</pre>"
      "<p style=\"color:#777;font-size:.85rem\">The REST API is still live — "
      "see <code>/api/status</code>.</p></body></html>";
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_status(req, "500 Internal Server Error");
  return httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
}

// ----------------------------------------------------------------------------
//  Startup / shutdown
// ----------------------------------------------------------------------------
static esp_err_t registerRoute(httpd_handle_t srv, const char *uri,
                               httpd_method_t method, esp_err_t (*handler)(httpd_req_t *),
                               bool websocket) {
  httpd_uri_t u;
  memset(&u, 0, sizeof(u));
  u.uri = uri;
  u.method = method;
  u.handler = handler;
  u.user_ctx = nullptr;
  if (websocket) {
    u.is_websocket = true;
    u.handle_ws_control_frames = false;
  }
  return httpd_register_uri_handler(srv, &u);
}

bool webServerInit() {
  if (g_webServer) return true;

  // LittleFS is the SPA's home (Phase 2 writes index.html.gz into it).
  // NOTE: on this core (pioarduino) the mountable subtype is 'spiffs' (0x40),
  // not 'fat' — same partition table shape as lora-relay (see partitions.csv).
  // Mount by PARTITION LABEL — the one-arg begin() picks a core-default label
  // that is not in our table (a failed mount just means / has no SPA to serve).
  // lora-relay's shape: (readOnly, path, maxOpen, label).
  if (!LittleFS.begin(true, "/littlefs", 10, "spa_a")) {
    LOGE("web: LittleFS mount FAILED — the SPA will NOT be served (/ shows the hint)");
  } else {
    LOGF("web: LittleFS up  (%lu KB total, %lu KB free)",
         (unsigned long)(LittleFS.totalBytes() / 1024),
         (unsigned long)((LittleFS.totalBytes() - LittleFS.usedBytes()) / 1024));
    loadSpaVersion();
  }

  // lora-relay's config values.
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = 80;
  cfg.max_uri_handlers = 32;
  cfg.task_priority = 2;
  cfg.stack_size = 8192;
  cfg.core_id = PRO_CPU_NUM;
  cfg.max_open_sockets = 12;
  cfg.lru_purge_enable = true;

  if (httpd_start(&g_webServer, &cfg) != ESP_OK) {
    g_webServer = nullptr;
    LOGE("web: httpd_start failed");
    return false;
  }

  // Route order matters: specific before the SPA catch-all (last).
  if (registerRoute(g_webServer, "/api/status", HTTP_GET, apiStatusHandler, false) != ESP_OK) goto fail;
  if (registerRoute(g_webServer, "/api/lights", HTTP_GET, apiLightsHandler, false) != ESP_OK) goto fail;
  if (registerRoute(g_webServer, "/ws/log", HTTP_GET, wsLogHandler, true) != ESP_OK) goto fail;
  if (!registerLightRoutes(g_webServer)) goto fail;
  if (!registerSystemRoutes(g_webServer)) goto fail;
  if (registerRoute(g_webServer, "/", HTTP_GET, indexHandler, false) != ESP_OK) goto fail;
  if (registerRoute(g_webServer, "/*", HTTP_GET, indexHandler, false) != ESP_OK) goto fail;

  LOGF("web: HTTP up on :80  (SPA + /api/* + WS /ws/log)");
  return true;

fail:
  httpd_stop(g_webServer);
  g_webServer = nullptr;
  LOGE("web: route registration failed");
  return false;
}

void webServerShutdown() {
  if (!g_webServer) return;
  httpd_stop(g_webServer);
  g_webServer = nullptr;
  LOGF("web: HTTP down");
}

bool webServerIsRunning() { return g_webServer != nullptr; }
