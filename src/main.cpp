// ============================================================================
//  BRMesh Controller — app shell
//
//  This file is the *application*: it owns setup()/loop() and wires the
//  engine (MeshController) to its front ends. The heavy lifting lives elsewhere:
//
//    * protocol.{h,cpp}  — the FastCon byte codec (pure, testable)
//    * mesh.{h,cpp}      — the engine: key + lights + BLE advertise/scan +
//                          discovery/keying + NVS persistence (MeshController class)
//    * console.{h,cpp}   — the serial console driver (POC UI)
//
//  Two build modes:
//    ENABLE_WIFI=0  -> serial console drives control frames directly (POC).
//    ENABLE_WIFI=1  -> each light is exposed as its own HA entity via MQTT,
//                      plus HTTP status/rescan. The HA + HTTP code below is
//                      that second front end — a peer of the console, not a
//                      replacement (both consume the same MeshController).
// ============================================================================

#include <Arduino.h>

#include "config.h"
#include "MeshController.h"
#include "console.h"

#if ENABLE_WIFI
  #include <WiFi.h>
  #include <ArduinoHA.h>
  #include <WebServer.h>
#endif

// ============================================================================
//  App instances: one engine, its drivers, and the (bridge-only) network bits.
// ============================================================================
MeshController g_mesh;
Console g_console(g_mesh);

#if ENABLE_WIFI
  WiFiClient client;
  HADevice device;
  HAMqtt* mqtt = nullptr;
  WebServer server;
#endif

// ============================================================================
//  HA light entity (bridge mode only) — a driver on top of MeshController
// ============================================================================
#if ENABLE_WIFI
void onStateCommand(bool state, HALight* sender) {
  LightDevice* l = g_mesh.findLightById(sender->uniqueId());
  if (!l) return;
  Serial.printf("HA state %s -> #%d\n", state ? "ON" : "OFF", l->number);
  g_mesh.state(l->number, state);
  sender->setState(state);
}
void onBrightnessCommand(uint8_t brightness, HALight* sender) {
  LightDevice* l = g_mesh.findLightById(sender->uniqueId());
  if (!l) return;
  Serial.printf("HA brightness %d -> #%d\n", brightness, l->number);
  g_mesh.brightness(l->number, brightness);
  sender->setBrightness(brightness);
}
void onColorTemperatureCommand(uint16_t temperature, HALight* sender) {
  LightDevice* l = g_mesh.findLightById(sender->uniqueId());
  if (!l) return;
  Serial.printf("HA colorTemp %d -> #%d\n", temperature, l->number);
  g_mesh.colorTemp(l->number, temperature);
  sender->setColorTemperature(temperature);
}
void onRGBColorCommand(HALight::RGBColor color, HALight* sender) {
  LightDevice* l = g_mesh.findLightById(sender->uniqueId());
  if (!l) return;
  Serial.printf("HA RGB %d,%d,%d -> #%d\n", color.red, color.green, color.blue, l->number);
  g_mesh.rgb(l->number, color.red, color.green, color.blue);
  sender->setRGBColor(color);
}

void createLightEntity(LightDevice& l) {
  const bool rgbw = (l.type[0] == 0xa1 && l.type[1] == 0xa8) || (l.type[0] == 0x2a && l.type[1] == 0xa8);
  const bool rgb  = (l.type[0] == 0xa0 && l.type[1] == 0xa8);

  HALight* light = nullptr;
  if (rgbw) {
    light = new HALight(l.id.c_str(),
                        HALight::BrightnessFeature | HALight::ColorTemperatureFeature | HALight::RGBFeature);
    light->onStateCommand(onStateCommand);
    light->onBrightnessCommand(onBrightnessCommand);
    light->onColorTemperatureCommand(onColorTemperatureCommand);
    light->onRGBColorCommand(onRGBColorCommand);
    light->setBrightnessScale(127);
    light->setBrightness(127);
  } else if (rgb) {
    light = new HALight(l.id.c_str(), HALight::BrightnessFeature | HALight::RGBFeature);
    light->onStateCommand(onStateCommand);
    light->onBrightnessCommand(onBrightnessCommand);
    light->onRGBColorCommand(onRGBColorCommand);
    light->setBrightnessScale(127);
    light->setBrightness(127);
  } else {
    light = new HALight(l.id.c_str());   // Smart - on/off only
    light->onStateCommand(onStateCommand);
  }
  light->setName(l.name.c_str());
  l.light = light;
  Serial.printf("  created HA light '%s' (id=%s)\n", l.name.c_str(), l.id.c_str());
}

// ---- HTTP status / rescan (bridge mode only) ----
void handleStatus() {
  String s = "BRMesh Controller\n\n";
  s += "Registered lights:\n";
  const auto& lights = g_mesh.lights();
  if (lights.empty()) s += "  (none)\n";
  for (auto &l : lights) {
    if (!l.isRegistered) continue;
    s += String("  #") + String(l.number) + " " + String(l.name.c_str());
    s += " [" + String(l.id.c_str()) + "]\n";
  }
  s += "\nGET /scan to re-run discovery\n";
  server.send(200, "text/plain", s);
}
void handleScan() {
  if (g_mesh.isScanning()) { server.send(200, "text/plain", "already scanning"); return; }
  g_mesh.requestScan();
  server.send(200, "text/plain", "scan requested");
}
#endif

// ============================================================================
//  Setup / loop
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(250);
  Serial.println("\n=== BRMesh Controller ===");
  Serial.printf("ENABLE_WIFI=%d\n", ENABLE_WIFI);

  if (LED_PIN >= 0) {
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH);   // on while setting up / discovering
  }

  // 1) Engine: BLE init + restore/generate the key.
  g_mesh.begin();
  {
    const uint8_t* k = g_mesh.key();
    Serial.printf("Mesh key: %02X %02X %02X %02X\n", k[0], k[1], k[2], k[3]);
  }

#if ENABLE_WIFI
  // 2) HA device + MQTT. Wire the engine's onRegistered hook to HA entity
  //    creation so both loadLights() and live keying spawn entities.
  byte mac[6];
  WiFi.macAddress(mac);
  device.setUniqueId(mac, sizeof(mac));
  device.setName("BRMesh");
  device.setManufacturer("BRMesh");
  device.setModel("BRMesh");
  mqtt = new HAMqtt(client, device);
  g_mesh.setOnRegistered(&createLightEntity);

  // 3) WiFi.
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.println("Connecting to WiFi..."); }
  Serial.print("WiFi IP: ");
  Serial.println(WiFi.localIP());
#endif

  // 4) Restore previously-registered lights (no re-keying).
  g_mesh.loadLights();

  // 5) Discover NEW factory-key bulbs and key them under our key.
  g_mesh.addLights(SCAN_DURATION);

  if (LED_PIN >= 0) digitalWrite(LED_PIN, LOW);

#if ENABLE_WIFI
  // 6) HTTP status / rescan.
  server.on("/", handleStatus);
  server.on("/scan", handleScan);
  server.begin();

  // 7) Start MQTT (publishes HA auto-discovery for every light).
  //    NOTE(BRIDGE_PLAN §7): entities must be created after mqtt->begin();
  //    the current ordering predates that — reorder here in the P0 bridge pass.
  mqtt->begin(MQTT_BROKER_ADDR, MQTT_PORT);
#endif

  Serial.println("Ready.");
  if (!ENABLE_WIFI) {
    Serial.println("Test mode: use the serial console (type 'help').");
    Serial.println("To key the bulb, power-cycle it, then type 'scan'.");
  }
  g_console.banner();
}

void loop() {
  // Engine: run any queued background scan (console 'scan' / HTTP /scan).
  g_mesh.loop();

  // Serial console (both modes).
  g_console.poll();

#if ENABLE_WIFI
  mqtt->loop();
  server.handleClient();
#endif
}
