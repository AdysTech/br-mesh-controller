// HaBridge — Phase 3 HA MQTT bridge (see HaBridge.h for the contract).
#include "HaBridge.h"

#include <WiFi.h>
#include <Network.h>
#include <string>

#include "config.h"
#include "web/ApiHttp.h"   // the tiny flat-JSON readers (apiJsonString / apiJsonInt)
#include "web/WebLog.h"    // LOGE / LOGF / LOGD

// ============================================================================
//  Small JSON helpers (no JSON lib on the board).
// ============================================================================
namespace {

/// Bounded JSON string escape (lora memory practice): copy s into out with the
/// minimal escapes (\" \\ \n \r \t, control bytes as \u00xx). Returns true and
/// NUL-terminates on success; false if it would not fit. Keeps discovery JSON to
/// ONE bounded buffer + a single snprintf instead of a chain of std::string
/// temporaries (each of which is a small heap alloc/free that fragments the
/// pool over a soak). Names are short (≤ ~15), so a small cap is plenty.
bool jesc(char* out, size_t cap, const char* in) {
  if (!out || cap == 0) return false;
  size_t o = 0;
  auto put = [&](const char* s, size_t n) {
    if (o + n > cap - 1) { o = cap; return; }   // overflow → stop, flag truncated
    memcpy(out + o, s, n);
    o += n;
  };
  for (const unsigned char* p = (const unsigned char*)in; *p && o < cap; ++p) {
    unsigned char c = *p;
    if (c == '"')           put("\\\"", 2);
    else if (c == '\\')     put("\\\\", 2);
    else if (c == '\n')     put("\\n", 2);
    else if (c == '\r')     put("\\r", 2);
    else if (c == '\t')     put("\\t", 2);
    else if (c < 0x20) { char t[8]; int n = snprintf(t, sizeof(t), "\\u%04x", (int)c); put(t, (size_t)n); }
    else                  put((char*)&c, 1);
  }
  if (o >= cap) return false;
  out[o] = 0;
  return true;
}

/// A light's retained state JSON — the exact shape HA's `schema:"json"` light
/// reads (and the shape HA itself sends to the /set topic: uppercase "ON"/"OFF"
/// and an object `{"r","g","b"}` colour). Two things were leaving the entity
/// "Unknown": the state was lowercase "on" (HA's wire convention is uppercase),
/// and both `color` and `color_temp` were sent together. Emit a clean state:
/// uppercase ON/OFF, brightness on the HA 0-255 scale (the wire is 0-127), and
/// ONLY the active colour attribute — the RGB object when colour_mode is "rgb",
/// mireds when it is "color_temp".
std::string lightStateJson(const LightDevice& l) {
  if (!l.on)
    return "{\"state\":\"OFF\"}";
  const int haBri = (int)l.brightness * 255 / 127;
  char buf[160];
  if (l.colorMode == 1) {
    // Active mode is RGB: send the colour-wheel value as the object form HA
    // uses, omit color_temp so HA never sees a conflicting colour.
    snprintf(buf, sizeof(buf),
             "{\"state\":\"ON\",\"brightness\":%d,"
             "\"color\":{\"r\":%d,\"g\":%d,\"b\":%d},\"color_mode\":\"rgb\"}",
             haBri, (int)l.rgbR, (int)l.rgbG, (int)l.rgbB);
  } else {
    // Active mode is colour temperature (the white LED): send mireds, omit the
    // colour. colourMode 0 (never set) defaults to the white LED.
    const int mireds = 1000000 / (l.ct ? (int)l.ct : 4000);
    snprintf(buf, sizeof(buf),
             "{\"state\":\"ON\",\"brightness\":%d,"
             "\"color_temp\":%d,\"color_mode\":\"color_temp\"}",
             haBri, mireds);
  }
  return buf;
}

/// Parse a colour from a command on the /set topic. HA is the sole publisher of
/// this topic, and its `schema:"json"` light sends the object form
/// {"color":{"r":..,"g":..,"b":..}} (verified against the broker's own /set
/// payload) — that is the only shape we accept.
bool parseColor(const String& body, uint8_t& r, uint8_t& g, uint8_t& b) {
  size_t p = body.indexOf("\"color\"");
  if (p == -1) return false;
  size_t colon = body.indexOf(':', p);
  if (colon == -1 || body[colon + 1] != '{') return false;   // object form only

  // readNum: from a key position, find its ':', then the next integer.
  auto readNum = [&](size_t keyPos, char* out, size_t outSz) -> bool {
    size_t c = body.indexOf(':', keyPos);
    if (c == -1) return false;
    size_t i = c + 1;
    while (i < body.length() && (body[i] == ' ' || body[i] == '\t')) i++;
    if (i < body.length() && body[i] == '-') i++;
    size_t s = i;
    while (i < body.length() && body[i] >= '0' && body[i] <= '9') i++;
    if (s == i) return false;
    size_t n = (i - s < outSz - 1) ? (i - s) : (outSz - 1);
    for (size_t k = 0; k < n; k++) out[k] = body[s + k];
    out[n] = 0;
    return true;
  };
  size_t pr = body.indexOf("\"r\"", p);
  size_t pg = body.indexOf("\"g\"", p);
  size_t pb = body.indexOf("\"b\"", p);
  if (pr == -1 || pg == -1 || pb == -1) return false;
  char rr[16], gg[16], bb[16];
  if (!readNum(pr, rr, sizeof(rr))) return false;
  if (!readNum(pg, gg, sizeof(gg))) return false;
  if (!readNum(pb, bb, sizeof(bb))) return false;
  r = (uint8_t)atoi(rr);
  g = (uint8_t)atoi(gg);
  b = (uint8_t)atoi(bb);
  return true;
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
  // Bounded single-snprintf build (lora memory practice) — no std::string
  // churn. Escape the two dynamic strings once into small buffers, then emit
  // the whole discovery document from ONE bounded buffer (safe on the 8 KB
  // MQTT task stack).
  char idE[48];    if (!jesc(idE, sizeof(idE), ctrlId_.c_str()))   idE[0] = 0;
  char nameE[48];  if (!jesc(nameE, sizeof(nameE), hubName_.c_str())) nameE[0] = 0;
  char base[64];   snprintf(base, sizeof(base), "brmesh/%s/", idE);
  char stateT[80]; snprintf(stateT, sizeof(stateT), "%sstate", base);
  // dev.cu is always the board's own SPA, derived live — a stored domain adds
  // nothing (the firewall routes by IP either way), and re-publish tracks IP changes.
  char cu[48];     snprintf(cu, sizeof(cu), "http://%s/", WiFi.localIP().toString().c_str());

  char buf[2048];
  // HA MQTT device-discovery schema (validated against HA core's abbreviations +
  // discovery schemas, HA ≥ 2026.4): device id is `ids` (a LIST), model is `mdl`,
  // `cmps`/components is a DICT keyed by a stable component id (not an array), and
  // each component names its platform with `p` (not `type`). Device-class is
  // `dev_cla`, unit is `unit_of_meas`, icon is `ic`. Full key names also accepted;
  // we use the abbreviations (smaller payloads).
  int n = snprintf(buf, sizeof(buf),
    "{\"o\":{\"name\":\"brmesh\",\"sw\":\"%s\"},"
    "\"dev\":{"
    "\"ids\":[\"%s\"],"
    "\"name\":\"%s\","
    "\"cu\":\"%s\","
    "\"mf\":\"Waveshare\",\"mdl\":\"ESP32-S3-Zero\",\"sw\":\"%s\"},"
    "\"avty\":[{\"t\":\"%sstatus\"}],\"avty_mode\":\"all\","
    "\"cmps\":{"
    "\"scan_btn\":{\"p\":\"button\",\"uniq_id\":\"%s_scan_btn\","
    "\"name\":\"Start scan\",\"cmd_t\":\"%sscan\"},"
    "\"scan\":{\"p\":\"sensor\",\"uniq_id\":\"%s_scan\",\"name\":\"Scan\",\"stat_t\":\"%s\","
    "\"val_tpl\":\"{{ value_json.scan }}\","
    "\"ent_cat\":\"diagnostic\",\"ic\":\"mdi:radar\"},"
    "\"lights\":{\"p\":\"sensor\",\"uniq_id\":\"%s_lights\",\"name\":\"Lights\",\"stat_t\":\"%s\","
    "\"val_tpl\":\"{{ value_json.lights }}\","
    "\"ent_cat\":\"diagnostic\",\"ic\":\"mdi:led-strip-variant\"},"
    "\"rssi\":{\"p\":\"sensor\",\"uniq_id\":\"%s_rssi\",\"name\":\"WiFi RSSI\",\"stat_t\":\"%s\","
    "\"val_tpl\":\"{{ value_json.rssi }}\",\"unit_of_meas\":\"dBm\","
    "\"ent_cat\":\"diagnostic\",\"ic\":\"mdi:wifi\"},"
    "\"mqtt\":{\"p\":\"sensor\",\"uniq_id\":\"%s_mqtt\",\"name\":\"MQTT link\",\"stat_t\":\"%s\","
    "\"val_tpl\":\"{{ value_json.mqtt }}\","
    "\"ent_cat\":\"diagnostic\",\"ic\":\"mdi:lan\"},"
    "\"queue\":{\"p\":\"sensor\",\"uniq_id\":\"%s_queue\",\"name\":\"Queue depth\",\"stat_t\":\"%s\","
    "\"val_tpl\":\"{{ value_json.queue }}\","
    "\"ent_cat\":\"diagnostic\",\"ic\":\"mdi:queue\"}}}",
    FIRMWARE_VERSION,
    idE, nameE, cu, FIRMWARE_VERSION,
    base,
    idE, base,
    idE, stateT,
    idE, stateT,
    idE, stateT,
    idE, stateT,
    idE, stateT);
  if (n < 0 || (size_t)n >= sizeof(buf))
    LOGE("MQTT: hub discovery JSON truncated (n=%d cap=%zu)", n, sizeof(buf));
  return buf;
}

std::string HaBridge::bulbDiscoveryJson(const LightDevice& l) const {
  // Bounded single-snprintf build (lora memory practice) — no std::string churn.
  char idE[48];    if (!jesc(idE, sizeof(idE), ctrlId_.c_str()))   idE[0] = 0;
  char nameE[48];  if (!jesc(nameE, sizeof(nameE), l.name.c_str())) nameE[0] = 0;
  char devId[64];  snprintf(devId, sizeof(devId), "%s_l%d", idE, (int)l.number);
  char base[64];   snprintf(base, sizeof(base), "brmesh/%s/", idE);

  char buf[768];
  // Same device-discovery schema as the hub: `ids` LIST, `mdl`, `cmps` a DICT
  // keyed by a component id, component platform under `p` (not `type`). The
  // light is `schema:"json"` — HA reads/writes native JSON keys (state,
  // brightness, color, color_temp, color_mode) with NO per-key templates.
  // supported_color_modes advertises BOTH rgb and color_temp so the whole gamut
  // (colour wheel + temp slider) always shows; min/max mireds bound the temp
  // (166–370 = 6000–2700 K, the bulbs' 2700–6000 K range).
  int n = snprintf(buf, sizeof(buf),
    "{\"o\":{\"name\":\"brmesh\",\"sw\":\"%s\"},"
    "\"dev\":{"
    "\"ids\":[\"%s\"],"
    "\"name\":\"%s\","
    "\"via_device\":\"%s\","
    "\"mf\":\"BroadLink\",\"mdl\":\"BRMesh bulb\"},"
    // Availability list: hub status (covered by the single LWT) + the bulb's
    // own topic. avty_mode "all" = both must be "online" → one LWT takes hub
    // AND every bulb offline at once.
    "\"avty\":[{\"t\":\"%sstatus\"},{\"t\":\"%sl%d/availability\"}],\"avty_mode\":\"all\","
    "\"cmps\":{\"light\":{\"p\":\"light\",\"name\":\"%s\","
    "\"uniq_id\":\"%s\","
    "\"stat_t\":\"%sl%d/state\","
    "\"cmd_t\":\"%sl%d/set\","
    "\"schema\":\"json\","
    "\"sup_clrm\":[\"rgb\",\"color_temp\"],"
    "\"min_mirs\":166,\"max_mirs\":370}}}",
    FIRMWARE_VERSION,
    devId, nameE, idE,
    base, base, (int)l.number,   // avty[0] = base+"status"; avty[1] = base+"l<N>/availability"
    nameE,                      // cmps.light.name (entity name = the SPA bulb name)
    devId,                      // cmps.light.uniq_id
    base, (int)l.number,        // stat_t = base+"l<N>/state"
    base, (int)l.number);       // cmd_t  = base+"l<N>/set"
  if (n < 0 || (size_t)n >= sizeof(buf))
    LOGE("MQTT: bulb #%u discovery JSON truncated (n=%d cap=%zu)", l.number, n, sizeof(buf));
  return buf;
}

void HaBridge::publishHub() {
  pub(discoveryTopic(ctrlId_), hubDiscoveryJson(), true);
  publishHubState();
}

void HaBridge::publishBulbDiscovery(const LightDevice& l) {
  pub(discoveryTopic(ctrlId_ + "_l" + std::to_string(l.number)), bulbDiscoveryJson(l), true);
}

void HaBridge::publishHubState() {
  char buf[160];
  snprintf(buf, sizeof(buf),
           "{\"scan\":\"%s\",\"lights\":%d,\"rssi\":%d,\"mqtt\":\"%s\","
           "\"queue\":%d,\"queue_overflow\":%lu}",
           mesh_.isScanning() ? "scanning" : "idle",
           mesh_.lightCount(),
           (int)WiFi.RSSI(),
           connected_ ? "online" : "offline",
           mesh_.queueDepth(),
           mesh_.queueOverflow());
  pub(hubStateTopic(), buf, true);
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
  // The CONNECTED event runs our discovery publish IN THIS task: hubDiscoveryJson
  // puts a 2 KB buffer on the stack, bulbDiscoveryJson 768 B, plus snprintf's own
  // frames, on top of esp-mqtt's frames — a ~3 KB peak. 16 KB leaves a wide margin
  // over that. (The earlier infinite-reboot crash was NOT the stack — it was a
  // missing snprintf %s arg in bulbDiscoveryJson; the stack was never the limit.)
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
    mesh_.requestScan(SCAN_DURATION);
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
  const String b(body.c_str());
  std::string st = apiJsonString(b, "state");
  for (auto& c : st) c = (char)tolower((unsigned char)c);   // HA may send ON/OFF
  const int bri = apiJsonInt(b, "brightness");      // -1 = absent (HA scale 0-255)
  const int mireds = apiJsonInt(b, "color_temp");   // -1 = absent (HA mireds)
  uint8_t r = 0, g = 0, bl = 0;
  const bool hasColor = parseColor(b, r, g, bl);
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
