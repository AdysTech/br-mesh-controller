// HaBridge — Phase 3 HA MQTT bridge (see HaBridge.h for the contract).
#include "HaBridge.h"

#include <WiFi.h>
#include <Network.h>
#include <string>

#include "config.h"
#include "settings.h"      // loadSettings() — the configured discovery window
#include <cJSON.h>         // ESP-IDF-bundled cJSON (framework libs) — parses the /set command
#include "web/WebLog.h"    // LOGE / LOGF / LOGD

// ============================================================================
//  JSON in + out. The incoming /set command is PARSED with the ESP-IDF-bundled
//  cJSON (see applyLightCommand); the emitted payloads (state + device
//  discovery) are BUILT as cJSON trees and serialized with
//  cJSON_PrintUnformatted (jsonOut below) — no hand-rolled escaping, no fixed
//  snprintf buffers on the stack (those become heap, freed one-shot).
// ============================================================================
namespace {

/// Serialize a fully-built cJSON tree into a compact (unformatted) std::string
/// and release it: free the print buffer (cJSON_free) and the tree
/// (cJSON_Delete). Returns "" on a null tree or a failed print. This is what the
/// discovery + state builders return — cJSON owns the escaping, and the heap is
/// one-shot (build → print → free) instead of fixed snprintf stack buffers.
std::string jsonOut(cJSON* root) {
  char* s = root ? cJSON_PrintUnformatted(root) : nullptr;
  if (!s) {
    if (root) cJSON_Delete(root);
    return {};
  }
  std::string out(s);
  cJSON_free(s);
  cJSON_Delete(root);
  return out;
}

/// A light's retained state JSON — the exact shape HA's `schema:"json"` light
/// reads (and the shape HA itself sends to the /set topic: uppercase "ON"/"OFF"
/// and an object `{"r","g","b"}` colour). Emit a clean state: uppercase ON/OFF,
/// brightness on the HA 0-255 scale (the wire is 0-127), and ONLY the active
/// colour attribute — the RGB object when colour_mode is "rgb", mireds when it
/// is "color_temp".
std::string lightStateJson(const LightDevice& l) {
  cJSON* j = cJSON_CreateObject();
  if (!j) return {};
  cJSON_AddStringToObject(j, "state", l.on ? "ON" : "OFF");
  if (l.on) {
    cJSON_AddNumberToObject(j, "brightness", (int)l.brightness * 255 / 127);
    if (l.colorMode == 1) {
      // Active mode is RGB: send the colour-wheel value as the object form HA
      // uses, omit color_temp so HA never sees a conflicting colour.
      cJSON* c = cJSON_AddObjectToObject(j, "color");
      cJSON_AddNumberToObject(c, "r", (int)l.rgbR);
      cJSON_AddNumberToObject(c, "g", (int)l.rgbG);
      cJSON_AddNumberToObject(c, "b", (int)l.rgbB);
      cJSON_AddStringToObject(j, "color_mode", "rgb");
    } else {
      // Active mode is colour temperature (the white LED): send mireds, omit the
      // colour. colourMode 0 (never set) defaults to the white LED.
      cJSON_AddNumberToObject(j, "color_temp", 1000000 / (l.ct ? (int)l.ct : 4000));
      cJSON_AddStringToObject(j, "color_mode", "color_temp");
    }
  }
  return jsonOut(j);
}

} // namespace

// ============================================================================
//  Topic builders
// ============================================================================
std::string HaBridge::statusTopic() const      { return "brmesh/" + ctrlId_ + "/status"; }
std::string HaBridge::hubStateTopic() const    { return "brmesh/" + ctrlId_ + "/state"; }
std::string HaBridge::scanTopic() const        { return "brmesh/" + ctrlId_ + "/scan"; }
std::string HaBridge::lightStateTopic(uint8_t n) const { return "brmesh/" + ctrlId_ + "/l" + std::to_string(n) + "/state"; }
std::string HaBridge::lightSetTopic(uint8_t n)  const { return "brmesh/" + ctrlId_ + "/l" + std::to_string(n) + "/set"; }
std::string HaBridge::lightAvtyTopic(uint8_t n) const { return "brmesh/" + ctrlId_ + "/l" + std::to_string(n) + "/availability"; }
std::string HaBridge::discoveryTopic(const std::string& uid) const { return "homeassistant/device/" + uid + "/config"; }

// ============================================================================
//  Publishing
// ============================================================================
void HaBridge::pub(const std::string& topic, const std::string& payload, bool retain) {
  if (!client_) return;
  int id = esp_mqtt_client_publish(client_, topic.c_str(), payload.c_str(),
                                   (int)payload.size(), 1, retain ? 1 : 0);
  if (id < 0)
    LOGE("MQTT: publish failed (%d) on %s", id, topic.c_str());
}

/// Empty retained payload on a topic = "remove this" (discovery purge,
/// retained-state clear).
void HaBridge::purge(const std::string& topic) {
  if (!client_) return;
  int id = esp_mqtt_client_publish(client_, topic.c_str(), nullptr, 0, 1, 1);
  if (id < 0)
    LOGE("MQTT: purge failed (%d) on %s", id, topic.c_str());
}

std::string HaBridge::hubDiscoveryJson() const {
  // HA MQTT device-discovery schema (validated against HA core's abbreviations +
  // discovery schemas, HA ≥ 2026.4): device id is `ids` (a LIST), model is `mdl`,
  // `cmps`/components is a DICT keyed by a stable component id (not an array), and
  // each component names its platform with `p` (not `type`). Device-class is
  // `dev_cla`, unit is `unit_of_meas`, icon is `ic`. We use the abbreviations
  // (smaller payloads). The whole document is a cJSON tree — escaping is cJSON's,
  // not ours.
  const std::string base   = "brmesh/" + ctrlId_ + "/";
  const std::string stateT = base + "state";
  // dev.cu is always the board's own SPA, derived live — a stored domain adds
  // nothing (the firewall routes by IP either way), and re-publish tracks IP changes.
  const std::string cu = "http://" + std::string(WiFi.localIP().toString().c_str()) + "/";

  cJSON* root = cJSON_CreateObject();
  if (!root) return {};

  // "o" — object (name + firmware).
  cJSON* o = cJSON_AddObjectToObject(root, "o");
  cJSON_AddStringToObject(o, "name", "brmesh");
  cJSON_AddStringToObject(o, "sw", FIRMWARE_VERSION);

  // "dev" — the hub device: id list, name, own-SPA URL, manufacturer/model/sw.
  cJSON* dev = cJSON_AddObjectToObject(root, "dev");
  cJSON* ids = cJSON_AddArrayToObject(dev, "ids");
  cJSON_AddItemToArray(ids, cJSON_CreateString(ctrlId_.c_str()));
  cJSON_AddStringToObject(dev, "name", hubName_.c_str());
  cJSON_AddStringToObject(dev, "cu", cu.c_str());
  cJSON_AddStringToObject(dev, "mf", "Waveshare");
  cJSON_AddStringToObject(dev, "mdl", "ESP32-S3-Zero");
  cJSON_AddStringToObject(dev, "sw", FIRMWARE_VERSION);

  // "avty" — hub status only (covered by the single LWT); avty_mode "all".
  cJSON* avty = cJSON_AddArrayToObject(root, "avty");
  cJSON* av0 = cJSON_CreateObject();
  cJSON_AddStringToObject(av0, "t", (base + "status").c_str());
  cJSON_AddItemToArray(avty, av0);
  cJSON_AddStringToObject(root, "avty_mode", "all");

  // "cmps" — components (a dict keyed by a stable component id).
  cJSON* cmps = cJSON_AddObjectToObject(root, "cmps");

  // scan_btn — the button that triggers a scan.
  cJSON* btn = cJSON_AddObjectToObject(cmps, "scan_btn");
  cJSON_AddStringToObject(btn, "p", "button");
  cJSON_AddStringToObject(btn, "uniq_id", (ctrlId_ + "_scan_btn").c_str());
  cJSON_AddStringToObject(btn, "name", "Start scan");
  cJSON_AddStringToObject(btn, "cmd_t", (base + "scan").c_str());

  // The five diagnostic sensors all read the hub state topic via value_json.<key>.
  auto sensor = [this, cmps, &stateT](const char* key, const char* suffix,
                                      const char* name, const char* ic,
                                      const char* unit) {
    cJSON* s = cJSON_AddObjectToObject(cmps, key);
    cJSON_AddStringToObject(s, "p", "sensor");
    cJSON_AddStringToObject(s, "uniq_id", (ctrlId_ + suffix).c_str());
    cJSON_AddStringToObject(s, "name", name);
    cJSON_AddStringToObject(s, "stat_t", stateT.c_str());
    const std::string tpl = std::string("{{ value_json.") + key + " }}";
    cJSON_AddStringToObject(s, "val_tpl", tpl.c_str());
    cJSON_AddStringToObject(s, "ent_cat", "diagnostic");
    cJSON_AddStringToObject(s, "ic", ic);
    if (unit) cJSON_AddStringToObject(s, "unit_of_meas", unit);
  };
  sensor("scan",   "_scan",   "Scan",        "mdi:radar",             nullptr);
  sensor("lights", "_lights", "Lights",      "mdi:led-strip-variant", nullptr);
  sensor("rssi",   "_rssi",   "WiFi RSSI",   "mdi:wifi",              "dBm");
  sensor("mqtt",   "_mqtt",   "MQTT link",   "mdi:lan",               nullptr);
  sensor("queue",  "_queue",  "Queue depth", "mdi:queue",             nullptr);

  return jsonOut(root);
}

std::string HaBridge::bulbDiscoveryJson(const LightDevice& l) const {
  // Same device-discovery schema as the hub: `ids` LIST, `mdl`, `cmps` a DICT
  // keyed by a component id, component platform under `p` (not `type`). The
  // light is `schema:"json"` — HA reads/writes native JSON keys (state,
  // brightness, color, color_temp, color_mode) with NO per-key templates.
  // supported_color_modes advertises BOTH rgb and color_temp so the whole gamut
  // (colour wheel + temp slider) always shows; min/max mireds bound the temp
  // (166–370 = 6000–2700 K, the bulbs' 2700–6000 K range).
  const std::string devId = ctrlId_ + "_l" + std::to_string(l.number);
  const std::string base  = "brmesh/" + ctrlId_ + "/";
  const std::string sub   = base + "l" + std::to_string(l.number) + "/";

  cJSON* root = cJSON_CreateObject();
  if (!root) return {};

  // "o" — object (name + firmware).
  cJSON* o = cJSON_AddObjectToObject(root, "o");
  cJSON_AddStringToObject(o, "name", "brmesh");
  cJSON_AddStringToObject(o, "sw", FIRMWARE_VERSION);

  // "dev" — the bulb device, chained to the hub (via_device).
  cJSON* dev = cJSON_AddObjectToObject(root, "dev");
  cJSON* ids = cJSON_AddArrayToObject(dev, "ids");
  cJSON_AddItemToArray(ids, cJSON_CreateString(devId.c_str()));
  cJSON_AddStringToObject(dev, "name", l.name.c_str());
  cJSON_AddStringToObject(dev, "via_device", ctrlId_.c_str());
  cJSON_AddStringToObject(dev, "mf", "BroadLink");
  cJSON_AddStringToObject(dev, "mdl", "BRMesh bulb");

  // Availability list: hub status (covered by the single LWT) + the bulb's own
  // topic. avty_mode "all" = both must be "online" → one LWT takes hub AND every
  // bulb offline at once.
  cJSON* avty = cJSON_AddArrayToObject(root, "avty");
  cJSON* av0 = cJSON_CreateObject();
  cJSON_AddStringToObject(av0, "t", (base + "status").c_str());
  cJSON* av1 = cJSON_CreateObject();
  cJSON_AddStringToObject(av1, "t", (sub + "availability").c_str());
  cJSON_AddItemToArray(avty, av0);
  cJSON_AddItemToArray(avty, av1);
  cJSON_AddStringToObject(root, "avty_mode", "all");

  // "cmps.light" — the light component.
  cJSON* light = cJSON_AddObjectToObject(cJSON_AddObjectToObject(root, "cmps"), "light");
  cJSON_AddStringToObject(light, "p", "light");
  cJSON_AddStringToObject(light, "name", l.name.c_str());
  cJSON_AddStringToObject(light, "uniq_id", devId.c_str());
  cJSON_AddStringToObject(light, "stat_t", (sub + "state").c_str());
  cJSON_AddStringToObject(light, "cmd_t", (sub + "set").c_str());
  cJSON_AddStringToObject(light, "schema", "json");
  cJSON* clrm = cJSON_AddArrayToObject(light, "sup_clrm");
  cJSON_AddItemToArray(clrm, cJSON_CreateString("rgb"));
  cJSON_AddItemToArray(clrm, cJSON_CreateString("color_temp"));
  cJSON_AddNumberToObject(light, "min_mirs", 166);
  cJSON_AddNumberToObject(light, "max_mirs", 370);

  return jsonOut(root);
}

void HaBridge::publishHub() {
  pub(discoveryTopic(ctrlId_), hubDiscoveryJson(), true);
  publishHubState();
}

void HaBridge::publishBulbDiscovery(const LightDevice& l) {
  pub(discoveryTopic(ctrlId_ + "_l" + std::to_string(l.number)), bulbDiscoveryJson(l), true);
}

void HaBridge::publishHubState() {
  cJSON* j = cJSON_CreateObject();
  if (!j) return;
  cJSON_AddStringToObject(j, "scan", mesh_.isScanning() ? "scanning" : "idle");
  cJSON_AddNumberToObject(j, "lights", (int)mesh_.lightCount());
  cJSON_AddNumberToObject(j, "rssi", (int)WiFi.RSSI());
  cJSON_AddStringToObject(j, "mqtt", connected_ ? "online" : "offline");
  cJSON_AddNumberToObject(j, "queue", (int)mesh_.queueDepth());
  cJSON_AddNumberToObject(j, "queue_overflow", (unsigned long)mesh_.queueOverflow());
  pub(hubStateTopic(), jsonOut(j), true);
}

void HaBridge::publishAll() {
  if (!client_) return;
  // Purge bulbs that were forgotten while we were offline (their discovery
  // would otherwise linger in the broker and HA would keep the device).
  uint8_t toPurge[8];
  int toPurgeCount = 0;
  if (purgeMux_) xSemaphoreTake(purgeMux_, portMAX_DELAY);
  toPurgeCount = pendingPurgeCount_;
  memcpy(toPurge, pendingPurge_, sizeof(toPurge));
  pendingPurgeCount_ = 0;
  if (purgeMux_) xSemaphoreGive(purgeMux_);
  for (int i = 0; i < toPurgeCount; i++)
    purgeBulb(toPurge[i]);

  if (!connected_) return;

  // 1) Availability first (birth), 2) hub, 3) every bulb.
  pub(statusTopic(), "online", true);
  publishHub();
  for (const auto& l : mesh_.lights()) {
    if (!l.isRegistered) continue;
    pub(lightAvtyTopic(l.number), "online", true);
    publishBulbDiscovery(l);
    pub(lightStateTopic(l.number), lightStateJson(l), true);
  }
  LOGF("MQTT: discovery + state published (hub + %d bulb(s))", mesh_.lightCount());
}

void HaBridge::purgeBulb(uint8_t n) {
  // Empty retained payloads remove the discovery entry and the retained state;
  // the bulb's availability rides in the hub status (already covered).
  purge(discoveryTopic(ctrlId_ + "_l" + std::to_string(n)));
  purge(lightStateTopic(n));
  LOGF("MQTT: purged bulb #%u from HA", n);
}

// ============================================================================
//  Client lifecycle
// ============================================================================
void HaBridge::begin(const Settings& s) {
  host_ = s.mqttHost;
  port_ = s.mqttPort;
  user_ = s.mqttUser;
  pass_ = s.mqttPass;
  hubName_ = s.mqttName.empty() ? s.name : s.mqttName;

  // Identity is broker-independent (chip MAC) — derive it even when the bridge
  // is disabled, so the console/UI can always show the HA device id.
  uint8_t mac[6];
  WiFi.macAddress(mac);
  {
    char buf[32];
    snprintf(buf, sizeof(buf), "brmesh_%02X%02X%02X", mac[3], mac[4], mac[5]);
    ctrlId_ = buf;
    // broker-side client id (mclient): settable; default is short + cheap —
    // brmesh-<last 4 MAC hex>. Distinct from ctrlId_ (the stable HA device id).
    if (!s.mqttClient.empty()) {
      clientId_ = s.mqttClient;
    } else {
      snprintf(buf, sizeof(buf), "brmesh-%02X%02X", mac[4], mac[5]);
      clientId_ = buf;
    }
    statusTopic_ = statusTopic();   // stable storage for the LWT's topic pointer
  }

  if (host_.empty()) {
    LOGF("MQTT: no broker configured (mhost empty) — HA bridge disabled (ctrl_id %s)", ctrlId_.c_str());
    return;
  }

  // The config's char* fields point into members — stable for the client's
  // lifetime. Static: one bridge per board, and it keeps ~1 KB off the stack.
  static esp_mqtt_client_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.broker.address.hostname = host_.c_str();
  cfg.broker.address.port = port_;
  cfg.broker.address.transport = MQTT_TRANSPORT_OVER_TCP;
  cfg.credentials.client_id = clientId_.c_str();
  if (!user_.empty()) cfg.credentials.username = user_.c_str();
  if (!pass_.empty()) cfg.credentials.authentication.password = pass_.c_str();
  // LWT: the hub goes "offline" (retained) — the one LWT that takes every
  // entity down, since each bulb's avty list includes the hub status topic.
  cfg.session.last_will.topic = statusTopic_.c_str();    // member: stable for the client's life
  cfg.session.last_will.msg = "offline";
  cfg.session.last_will.msg_len = 0;
  cfg.session.last_will.qos = 1;
  cfg.session.last_will.retain = 1;
  cfg.session.keepalive = 30;
  cfg.network.reconnect_timeout_ms = 10000;
  cfg.task.priority = 4;
  // The CONNECTED event runs our discovery publish IN THIS task. The JSON builders
  // allocate their cJSON trees + print buffers on the heap (freed one-shot via
  // jsonOut), not on the stack, so this task's peak is esp-mqtt's own frames plus
  // small frames for the build loop — well within 16 KB. (The earlier
  // infinite-reboot crash was NOT the stack — it was a missing snprintf %s arg in
  // bulbDiscoveryJson; the stack was never the limit.)
  cfg.task.stack_size = 16384;
  cfg.buffer.size = 4096;
  cfg.buffer.out_size = 4096;

  client_ = esp_mqtt_client_init(&cfg);
  if (!client_) {
    LOGE("MQTT: client init failed — HA bridge disabled");
    return;
  }
  // Initialize the bridge's own state BEFORE start(): esp_mqtt_client_start()
  // spawns the client task, which can connect and fire CONNECTED -> publishAll()
  // before begin() returns. A fast first connect must already see enabled_ set
  // and purgeMux_ non-null (publishAll derefs the mutex under it).
  enabled_ = true;
  purgeMux_ = xSemaphoreCreateMutex();
  if (esp_mqtt_client_register_event(client_, MQTT_EVENT_ANY, onEvent, this) != ESP_OK)
    LOGE("MQTT: event registration failed");
  esp_mqtt_client_start(client_);
  LOGF("MQTT: connecting to %s:%u as '%s' (ctrl_id %s)",
       host_.c_str(), (unsigned)port_, clientId_.c_str(), ctrlId_.c_str());
}

void HaBridge::subscribeAll() {
  if (!client_) return;
  esp_mqtt_client_subscribe(client_, scanTopic().c_str(), 1);
  // One wildcard covers every bulb (current + future): brmesh/<ctrl_id>/+/set.
  esp_mqtt_client_subscribe(client_,
                            ("brmesh/" + ctrlId_ + "/+/set").c_str(), 1);
  // HA (re)announcing itself: trigger the full re-publish.
  esp_mqtt_client_subscribe(client_, "homeassistant/status", 1);
}

void HaBridge::onEvent(void* arg, esp_event_base_t base, int32_t id, void* data) {
  (void)base;
  auto* self = static_cast<HaBridge*>(arg);
  const auto* e = static_cast<const esp_mqtt_event_t*>(data);
  if (!e) return;
  switch (e->event_id) {
    case MQTT_EVENT_CONNECTED:
      self->connected_ = true;
      self->connectFails_ = 0;
      self->stopped_ = false;
      self->subscribeAll();
      self->publishAll();
      break;
    case MQTT_EVENT_ERROR:
    case MQTT_EVENT_DISCONNECTED:
      self->connected_ = false;
      if (!self->stopped_ && self->connectFails_ < 250) self->connectFails_++;
      break;
    case MQTT_EVENT_DATA:
      self->handleData(e);
      break;
    default:
      break;
  }
}

// ============================================================================
//  Incoming data: scan command, light set, HA online
// ============================================================================
void HaBridge::handleData(const esp_mqtt_event_t* e) {
  const std::string topic = e->topic
      ? std::string(e->topic, e->topic_len > 0 ? (size_t)e->topic_len : 0)
      : std::string();
  const std::string payload = e->data
      ? std::string(e->data, e->data_len > 0 ? (size_t)e->data_len : 0)
      : std::string();

  if (topic == "homeassistant/status" && payload.rfind("online", 0) == 0) {
    LOGF("MQTT: homeassistant/status=online — re-publishing discovery + state");
    publishAll();
    return;
  }
  if (topic == scanTopic()) {
    LOGF("MQTT: scan requested from HA");
    mesh_.requestScan(loadSettings().scanSec);  // the operator-configured window
    return;
  }
  // brmesh/<ctrl_id>/l<n>/set — extract <n>.
  const std::string prefix = "brmesh/" + ctrlId_ + "/l";
  if (topic.rfind(prefix, 0) == 0 && topic.size() > prefix.size() + 4 &&
      topic.compare(topic.size() - 4, 4, "/set") == 0) {
    char* endp = nullptr;
    const long n = strtol(topic.c_str() + prefix.size(), &endp, 10);
    if (n >= 1 && n <= 255 && (endp && *endp == '/'))
      applyLightCommand((uint8_t)n, payload);
  }
}

/// HA light command (schema:json): {"state":"ON|OFF|TOGGLE","brightness":0-255,
/// "color":{"r":..,"g":..,"b":..} (object form),
/// "color_temp":<mireds>,"color_mode":"rgb"|"color_temp"}.
/// Fields are INDEPENDENT — brightness is applied alongside colour/CT (HA sends
/// them together). Order: state, then brightness, then colour (wins) or CT.
void HaBridge::applyLightCommand(uint8_t n, const std::string& body) {
  cJSON* root = cJSON_Parse(body.c_str());
  if (!root) {
    LOGE("MQTT: set #%u — unparseable JSON: %.120s", n, body.c_str());
    return;
  }
  // HA `schema:"json"` sends uppercase "ON"/"OFF" (or "TOGGLE"); normalise.
  std::string st;
  if (const cJSON* s = cJSON_GetObjectItem(root, "state");
      s && cJSON_IsString(s) && s->valuestring) {
    st = s->valuestring;
    for (auto& c : st) c = (char)tolower((unsigned char)c);
  }
  const cJSON* briItem = cJSON_GetObjectItem(root, "brightness");   // -1 = absent (HA 0-255)
  const cJSON* ctItem  = cJSON_GetObjectItem(root, "color_temp");   // -1 = absent (mireds)
  const int bri    = (briItem && cJSON_IsNumber(briItem)) ? (int)briItem->valuedouble : -1;
  const int mireds = (ctItem  && cJSON_IsNumber(ctItem))  ? (int)ctItem->valuedouble  : -1;
  uint8_t r = 0, g = 0, bl = 0;
  bool hasColor = false;
  if (const cJSON* col = cJSON_GetObjectItem(root, "color"); col && cJSON_IsObject(col)) {
    auto pick = [](const cJSON* o, const char* k) -> int {
      const cJSON* v = cJSON_GetObjectItem(o, k);
      return (v && cJSON_IsNumber(v)) ? (int)v->valuedouble : -1;
    };
    const int ri = pick(col, "r"), gi = pick(col, "g"), bi = pick(col, "b");
    if (ri >= 0 && gi >= 0 && bi >= 0) {
      r = (uint8_t)ri; g = (uint8_t)gi; bl = (uint8_t)bi; hasColor = true;
    }
  }
  cJSON_Delete(root);
  LOGD("MQTT: set #%u state=%s bri=%d mireds=%d rgb=%d",
       n, st.c_str(), bri, mireds, (int)hasColor);

  bool on;
  if (st == "toggle") {
    LightDevice* l = mesh_.findLightByNumber(n);
    on = (l ? !l->on : true);
  } else if (st == "off") {
    on = false;
  } else {
    on = true;    // "on", or no explicit state but a field is present
  }
  mesh_.state(n, on);
  if (on) {
    if (bri >= 0)
      mesh_.brightness(n, (uint8_t)(bri * 127 / 255));
    if (hasColor) {
      mesh_.rgb(n, r, g, bl);
    } else if (mireds > 0) {
      uint32_t k = 1000000UL / (uint32_t)mireds;
      if (k < 2700) k = 2700;
      if (k > 6000) k = 6000;
      mesh_.colorTemp(n, (uint16_t)k);
    }
  }
  // ESP→HA: HA only ever sees the state we publish — a command does not itself
  // echo back. Re-assert the fresh believed state to the retained state topic
  // right after applying it: this pulls the entity out of "Unknown", keeps the
  // popup slider/colour in sync, and lets the popup on/off (a toggle against a
  // live baseline) track. loop() also re-asserts on a cadence as a safety net
  // against a dropped publish.
  if (LightDevice* l = mesh_.findLightByNumber(n))
    pub(lightStateTopic(n), lightStateJson(*l), true);
}

// ============================================================================
//  MeshController hooks
// ============================================================================
void HaBridge::bulbRegistered(LightDevice& l) {
  if (!enabled_) return;
  if (connected_) {
    pub(lightAvtyTopic(l.number), "online", true);
    publishBulbDiscovery(l);
    pub(lightStateTopic(l.number), lightStateJson(l), true);
  }
  LOGF("MQTT: bulb #%u '%s' available in HA", l.number, l.name.c_str());
}

void HaBridge::bulbRenamed(uint8_t number) {
  if (!enabled_ || !connected_) return;
  if (LightDevice* l = mesh_.findLightByNumber(number)) {
    publishBulbDiscovery(*l);   // new dev.name; ids/uniq_id unchanged
    LOGF("MQTT: re-published discovery for #%u ('%s')", number, l->name.c_str());
  }
}

void HaBridge::bulbForgotten(uint8_t number) {
  if (!enabled_) return;
  if (connected_) {
    purgeBulb(number);
    return;
  }
  // Offline: queue the purge for the next publishAll().
  if (!purgeMux_) return;
  xSemaphoreTake(purgeMux_, portMAX_DELAY);
  bool dup = false;
  for (int i = 0; i < pendingPurgeCount_; i++)
    if (pendingPurge_[i] == number) dup = true;
  if (!dup && pendingPurgeCount_ < (int)(sizeof(pendingPurge_) / sizeof(pendingPurge_[0])))
    pendingPurge_[pendingPurgeCount_++] = number;
  xSemaphoreGive(purgeMux_);
  LOGF("MQTT: purge of #%u deferred (offline)", number);
}

void HaBridge::notifyScanState(bool scanning) {
  if (!enabled_ || !connected_) return;
  // Re-reads isScanning() itself — the flag is already set by the time this
  // fires (it's driven from MeshController::loop() at the exact transition).
  publishHubState();
  LOGF("MQTT: hub state refreshed (scan %s)", scanning ? "scanning" : "idle");
}

// ============================================================================
//  Loop + settings
// ============================================================================
void HaBridge::loop() {
  // Reconnect backoff: esp-mqtt retries a failed connect every
  // reconnect_timeout_ms (10 s) forever — after MQTT_FAST_RETRIES consecutive
  // failures take the client down so the log quiets, then restart it once the
  // cooldown has elapsed. stop()/start() block on the client task, so they run
  // here (app context), NEVER from the event handler (client-task context).
  if (enabled_ && client_ && !stopped_ && connectFails_ >= MQTT_FAST_RETRIES) {
    esp_mqtt_client_stop(client_);
    stopped_ = true;
    nextRestartMs_ = millis() + MQTT_RECONNECT_COOLDOWN_MS;
    LOGF("MQTT: %u failed connects — backing off %lu s, next attempt then",
         (unsigned)connectFails_, (unsigned long)(MQTT_RECONNECT_COOLDOWN_MS / 1000UL));
    return;
  }
  if (enabled_ && client_ && stopped_ && (uint32_t)(millis() - nextRestartMs_) <= 1) {
    stopped_ = false;
    connectFails_ = 0;
    esp_mqtt_client_start(client_);
    LOGF("MQTT: cooldown over — reconnecting to %s:%u", host_.c_str(), (unsigned)port_);
    return;
  }

  if (!enabled_ || !connected_) return;
  const uint32_t now = millis();
  if (now - lastHubStateMs_ < 30000UL) return;
  lastHubStateMs_ = now;
  publishHubState();
  // Safety net: re-assert every registered bulb's believed state on the same
  // cadence. The connect-time publishAll() can land before HA's entity is ready,
  // leaving it "Unknown" until this live re-assert (or the next command) reaches
  // it. A handful of small retained messages per 30 s is negligible, and it
  // self-heals any dropped state.
  for (const auto& l : mesh_.lights()) {
    if (!l.isRegistered) continue;
    pub(lightStateTopic(l.number), lightStateJson(l), true);
  }
}

void HaBridge::updateSettings(const Settings& s) {
  const std::string newName = s.mqttName.empty() ? s.name : s.mqttName;
  const bool identityChanged = (newName != hubName_);
  const bool brokerChanged = (s.mqttHost != host_) || (s.mqttPort != port_) ||
                             (s.mqttUser != user_) || (s.mqttPass != pass_);
  hubName_ = newName;
  if (brokerChanged)
    LOGF("MQTT: broker settings changed — active from next boot");
  else if (identityChanged && enabled_ && connected_)
    publishHub();   // dev.name refreshed, ids unchanged
}
