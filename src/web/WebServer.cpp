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
  char buf[768];
  int n = snprintf(buf, sizeof(buf),
      "{\"name\":\"%s\","
      "\"mode\":\"sta\","
      "\"ip\":\"%s\","
      "\"ssid\":\"%s\","
      "\"rssi\":%d,"
      "\"mqtt\":%s,"
      "\"mqtt_enabled\":%s,"
      "\"ctrl_id\":\"%s\","
      "\"lights\":%d,"
      "\"scanning\":%s,"
      "\"queue_depth\":%d,"
      "\"queue_overflow\":%lu,"
      "\"fw\":\"%s\","
      "\"spa\":\"%s\","
      "\"board\":\"%s\","
      "\"uptime_s\":%lu,"
      "\"heap_free\":%lu,"
      "\"free_heap_kb\":%lu,"
      "\"min_free_heap_kb\":%lu,"
      "\"largest_free_internal_block_kb\":%lu,"
      "\"psram_size_kb\":%lu,"
      "\"psram_free_kb\":%lu}",
      jsonEscape(s.name.c_str()).c_str(),
      WiFi.localIP().toString().c_str(),
      jsonEscape(s.ssid.c_str()).c_str(),
      (int)WiFi.RSSI(),
      g_bridge.connected() ? "true" : "false",
      g_bridge.enabled() ? "true" : "false",
      g_bridge.ctrlId().empty() ? "" : g_bridge.ctrlId().c_str(),
      g_mesh.lightCount(),
      g_mesh.isScanning() ? "true" : "false",
      g_mesh.queueDepth(),
      (unsigned long)g_mesh.queueOverflow(),
      FIRMWARE_VERSION,
      jsonEscape(webSpaVersion()).c_str(),
      kBoardModel,
      (unsigned long)(millis() / 1000),
      (unsigned long)ESP.getFreeHeap(),
      (unsigned long)(esp_get_free_heap_size() / 1024),
      (unsigned long)(esp_get_minimum_free_heap_size() / 1024),
      (unsigned long)((uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
      (unsigned long)(ESP.getPsramSize() / 1024),
      (unsigned long)(ESP.getFreePsram() / 1024));
  if (n < 0 || (size_t)n >= sizeof(buf)) { apiSendError(req, 500, "status build failed"); return ESP_FAIL; }
  apiSendJson(req, 200, buf);
  return ESP_OK;
}

// ----------------------------------------------------------------------------
//  GET /api/lights
// ----------------------------------------------------------------------------
static esp_err_t apiLightsHandler(httpd_req_t *req) {
  const auto &lights = g_mesh.lights();
  std::string out = "[";
  for (size_t i = 0; i < lights.size(); i++) {
    const LightDevice &l = lights[i];
    // mode drives the UI picker: white (on, no colour), rgb, or ct.
    const char *mode =
        l.colorMode == 2 ? "ct" : (l.colorMode == 1 ? "rgb" : "white");
    char item[400];
    snprintf(item, sizeof(item),
        "%s{\"n\":%u,"
        "\"name\":\"%s\","
        "\"id\":\"%s\","
        "\"mac\":\"%s\","
        "\"type\":\"%02X%02X\","
        "\"features\":[\"on_off\",\"brightness\",\"rgb\",\"ct\"],"
        "\"min_kelvin\":2700,"
        "\"max_kelvin\":6000,"
        "\"believed_state\":{\"on\":%s,"
        "\"brightness\":%u,"
        "\"rgb\":[%u,%u,%u],"
        "\"ct\":%u,"
        "\"mode\":\"%s\"}}",
        i ? "," : "",
        (unsigned)l.number,
        jsonEscape(l.name.c_str()).c_str(),
        jsonEscape(l.id.c_str()).c_str(),
        jsonEscape(l.address.c_str()).c_str(),
        l.type[0], l.type[1],
        l.on ? "true" : "false",
        (unsigned)l.brightness,
        (unsigned)l.rgbR, (unsigned)l.rgbG, (unsigned)l.rgbB,
        (unsigned)l.ct,
        mode);
    out += item;
  }
  out += "]";
  apiSendJson(req, 200, out.c_str());
  return ESP_OK;
}

// ----------------------------------------------------------------------------
//  GET / (+ /*) — the SPA, or the embedded fallback while Phase 2 hasn't
//  flashed index.html.gz into LittleFS.
// ----------------------------------------------------------------------------
static const char s_fallbackHtml[] =
    "<!DOCTYPE html>\n"
    "<html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>BRMesh</title>"
    "<style>"
    "body{font-family:system-ui,sans-serif;background:#111;color:#ddd;"
    "margin:2rem auto;max-width:40rem;padding:0 1rem}"
    "h1{font-size:1.3rem}"
    "table{width:100%;border-collapse:collapse;margin-top:1rem}"
    "td{padding:.35rem .5rem;border-bottom:1px solid #333}"
    "td:first-child{color:#888}"
    ".note{color:#777;font-size:.85rem;margin-top:2rem}"
    "</style></head><body>"
    "<h1>BRMesh <span id=\"name\"></span></h1>"
    "<table id=\"kv\"></table>"
    "<p>lights: <span id=\"lights\">&hellip;</span></p>"
    "<p class=\"note\">Embedded Phase-1 status page. The Svelte SPA (Phase 2)"
    " replaces this once <code>index.html.gz</code> is in LittleFS.</p>"
    "<script>\n"
    "async function ref(){try{"
    "var s=await (await fetch('/api/status',{cache:'no-store'})).json();"
    "document.getElementById('name').textContent=s.name;"
    "var kv=document.getElementById('kv');kv.innerHTML='';"
    "['mode','ip','ssid','rssi','mqtt','lights','scanning','fw','board','uptime_s','heap_free']"
    ".forEach(function(k){kv.insertAdjacentHTML('beforeend',"
    "'<tr><td>'+k+'</td><td>'+JSON.stringify(s[k])+'</td></tr>');});"
    "var l=await (await fetch('/api/lights',{cache:'no-store'})).json();"
    "document.getElementById('lights').textContent="
    "(l.map(function(x){return '#'+x.n+' '+x.name+' '+x.mac;})).join(', ')||'(none)';"
    "}catch(e){document.getElementById('kv').innerHTML='<tr><td>offline</td></tr>';}}"
    "ref();setInterval(ref,5000);\n"
    "</script></body></html>";

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

  // 2) Embedded fallback page (Phase 1).
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, s_fallbackHtml, HTTPD_RESP_USE_STRLEN);
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
  // that is not in our table (mount fails → SPA path falls back to the inline
  // HTML). lora-relay's shape: (readOnly, path, maxOpen, label).
  if (!LittleFS.begin(true, "/littlefs", 10, "spa_a")) {
    LOGE("web: LittleFS mount FAILED — SPA path will use the embedded fallback");
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
