// ============================================================================
//  BRMesh Controller — app shell
//
//  This file is the *application*: it owns setup()/loop() and wires the
//  engine (MeshController) to its front ends. The heavy lifting lives elsewhere:
//
//    * protocol.{h,cpp}  — the FastCon byte codec (pure, testable)
//    * MeshController.*  — the engine: key + lights + BLE advertise/scan +
//                          discovery/keying + NVS persistence
//    * console.{h,cpp}   — the serial console driver
//
//  BRIDGE_PLAN §5 boot flow: NVS creds -> bounded STA attempt -> Improv BLE
//  provisioning fallback. Front ends wired here: serial console, web shell
//  (Phase 1), HA/MQTT bridge (Phase 3, HaBridge).
//
//  Provisioning rule: on a provisioning boot the mesh engine is NEVER started
//  — Improv owns the BLE radio for that boot, and provisioning always ends in
//  a reboot, so Improv and MeshController never coexist (and never share a
//  boot). A pre-provisioned board skips Improv and reaches the engine.
// ============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <Network.h>
#include <ESPmDNS.h>
#include <ImprovWiFiBLE.h>

#include "config.h"
#include "MeshController.h"
#include "HaBridge.h"
#include "console.h"
#include "settings.h"
#include "statusled.h"
#include "web/WebLog.h"
#include "web/WebServer.h"

// ============================================================================
//  App instances: one engine + its serial driver + Improv (provisioning).
// ============================================================================
MeshController g_mesh;
Console g_console(g_mesh);
ImprovWiFiBLE g_improv;
HaBridge g_bridge(g_mesh);     // Phase 3 — console/API reference it via extern

static bool     s_provisioning = false;
static uint32_t s_provStarted  = 0;

// BLE name: "BRMesh-" + the chip MAC's last two octets (identifies the unit).
static const char* improvName() {
  static char name[16];
  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(name, sizeof(name), "BRMesh-%02X%02X", mac[4], mac[5]);
  return name;
}

// P0.3 — re-entry: console `prov` (later web "Provision via BLE"):
// clear the saved WiFi creds -> reboot into the Improv branch.
void requestProvisioning() {
  LOGF("prov: clearing saved WiFi creds, rebooting into Improv");
  clearWiFiCreds();
  WiFi.persistent(false);   // keep the WiFi driver's own NVS slot out of play
  WiFi.disconnect(true);    // and wipe it if present
  delay(300);
  ESP.restart();
}

// P0.x — console `hostname <name>` (Tasmota-style SetHostName): persist the
// device name (NVS "name", already used at boot for the DHCP hostname) and
// apply it to the live netif — the core only snapshots the stored name onto
// the STA netif inside the first WiFi.mode(WIFI_STA) of a boot, so a bare
// WiFi.setHostname() would not reach an already-connected interface.
void setDeviceName(const char* name) {
  Settings s = loadSettings();
  s.name = name;
  saveSettings(s);
  NetworkInterface* def = Network.getDefaultInterface();
  if (def && def->setHostname(name)) {
    LOGF("hostname '%s' saved + applied (DHCP lease picks it up at renewal/reconnect)", name);
    return;
  }
  LOGF("hostname '%s' saved — active from the next boot/connect", name);
}

// Start the Improv BLE service. The brmesh NVS settings are the single
// source of truth for creds: a custom connect handler saves them (bypassing
// the library's own driver-NVS connect path) and reboots, so the very next
// boot goes straight at STA with the new creds.
void launchProvisioning() {
  s_provisioning = true;
  s_provStarted  = millis();
  ledSet(LED_AP);
  LOGF("=== Improv provisioning (BLE) ===");
  LOGF("  BLE name : %s", improvName());
  LOGF("  Phone/PC : open https://www.improv-wifi.com (Chrome/Edge) and scan");
  LOGF("  Window   : %lu s, then reboot to retry", PROV_TIMEOUT_MS / 1000);

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);    // wipe any stale driver-NVS creds

  g_improv.setCustomConnectWiFi([](const char* ssid, const char* pass) -> bool {
    LOGF("Improv: creds received (ssid=%s) — saving to NVS", ssid);
    Settings s = loadSettings();
    s.ssid = ssid;
    s.pass = pass ? pass : "";
    saveSettings(s);
    delay(1000);            // let the phone read the PROVISIONED state
    ESP.restart();
    return true;            // unreachable
  });

  // setDeviceInfo() initializes NimBLE and starts the Improv advertising.
  g_improv.setDeviceInfo(ImprovTypes::ChipFamily::CF_ESP32,
                         improvName(), FIRMWARE_VERSION, improvName());
  LOGF("Improv: ready. Serial console still available (type 'help').");
}

// ============================================================================
//  Setup / loop
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(250);
  LOGF("=== BRMesh Controller ===");
  LOGF("fw: %s (%s)", FIRMWARE_VERSION, kBoardModel);

  ledInit();
  ledSet(LED_BOOT);

  // Ensure the optional Phase-3 NVS keys exist (empty) before the first
  // loadSettings() — Preferences logs an ESP_ERROR per missing key, and
  // /api/status is polled (see seedSettings in settings.cpp).
  seedSettings();

  // ---- BRIDGE_PLAN §5 boot flow: NVS creds -> bounded STA -> Improv -------
  // The mesh engine is deliberately NOT started on a provisioning boot (see
  // the file header): Improv owns the BLE radio for that boot.
  const Settings s = loadSettings();
  bool wifiUp = false;
  if (!s.ssid.empty()) {
    WiFi.persistent(false);   // the driver's own NVS STA slot must not win
    // ORDER MATTERS: the core copies the stored name onto the STA netif INSIDE
    // WiFi.mode() (WiFiGenericClass::mode() -> esp_netif_set_hostname()), so
    // setHostname() must run BEFORE mode() — after that it only writes a static
    // buffer that is never applied to the netif again.
    WiFi.setHostname(s.name.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.begin(s.ssid.c_str(), s.pass.c_str());
    LOGF("WiFi: connecting to '%s' (timeout %lu s)...",
         s.ssid.c_str(), (unsigned long)WIFI_CONNECT_TIMEOUT_MS / 1000);
    const uint32_t t0 = millis();
    while (millis() - t0 < WIFI_CONNECT_TIMEOUT_MS) {
      if (WiFi.status() == WL_CONNECTED) break;
      delay(250);
    }
    wifiUp = (WiFi.status() == WL_CONNECTED);
    if (wifiUp) {
      LOGF("WiFi: connected, IP %s (rssi %d)", WiFi.localIP().toString().c_str(),
           (int)WiFi.RSSI());
      ledSet(LED_WIFI);
    }
  }
  if (!wifiUp) {
    LOGF("WiFi: not configured or connect failed -> provisioning");
    launchProvisioning();
    g_console.banner();
    return;   // loop(): console + LED + window timeout only
  }

  // 1) Engine: BLE init + restore/generate the mesh key.
  g_mesh.begin();
  {
    const uint8_t* k = g_mesh.key();
    LOGF("Mesh key: %02X %02X %02X %02X", k[0], k[1], k[2], k[3]);
  }

  // 2) Network shell (Phase 1, BRIDGE_PLAN §5: mDNS -> httpd -> loadLights):
  //    <name>.local on the LAN + the HTTP/WS server, then the light list.
  if (MDNS.begin(s.name.c_str())) {
    MDNS.addService("http", "tcp", 80);
    LOGF("mDNS: serving http on %s.local", s.name.c_str());
  } else {
    LOGF("mDNS: registration failed (http still up on the LAN IP)");
  }
  if (!webServerInit())
    LOGF("web: HTTP server failed to start — console-only mode");

  // 3) HA bridge (Phase 3): wire the driver hooks, restore the light list, THEN
  //    start MQTT. loadLights() fires onRegistered per light, but bulbRegistered
  //    is a no-op until begin() sets enabled_ — so nothing publishes early. The
  //    FIRST publishAll() on MQTT connect then sees the full restored list and
  //    publishes the hub state (real light count) + every bulb in one pass.
  //    Restoring BEFORE begin() is what fixes the first-boot "Lights: 0".
  g_mesh.setOnRegistered([](LightDevice& l) { g_bridge.bulbRegistered(l); });
  g_mesh.setOnRenamed([](uint8_t n) { g_bridge.bulbRenamed(n); });
  g_mesh.setOnForgotten([](uint8_t n) { g_bridge.bulbForgotten(n); });
  g_mesh.setOnScanState([](bool scanning) { g_bridge.notifyScanState(scanning); });
  g_mesh.loadLights();                 // restore NVS lights (no publish yet)
  g_bridge.begin(s);                   // MQTT connect -> publishAll (full list + count)

  LOGF("Ready. Web: http://%s.local  —  serial console: 'help'.", s.name.c_str());
  LOGF("To key the bulb, power-cycle it, then type 'scan 12'.");
  g_console.banner();
}

// ============================================================================
//  WiFi watchdog — the boot STA flow runs only ONCE. The driver's own
//  background reconnect gives up and the board then sits with a dead network
//  until a manual reboot, so loop() owns reconnection:
//  rate-limited (30 s), logged to the ring (visible on /ws/log), LED ERR
//  while retrying, back to WIFI on recovery.
// ============================================================================
static unsigned long s_wifiLastTry = 0;
static bool          s_wifiRetryArmed = false;

static void wifiWatchdog() {
  if (WiFi.status() == WL_CONNECTED) {
    if (s_wifiRetryArmed) {
      s_wifiRetryArmed = false;
      ledSet(LED_WIFI);
      LOGF("WiFi: reconnected, IP %s (rssi %d)",
           WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
    }
    return;
  }

  if (millis() - s_wifiLastTry < 30000UL) return;   // rate-limit
  s_wifiLastTry = millis();
  s_wifiRetryArmed = true;
  ledSet(LED_ERR);
  LOGF("WiFi: link lost — reconnecting…");
  const Settings s = loadSettings();   // fresh creds (console `set` may have changed them)
  WiFi.disconnect();
  WiFi.begin(s.ssid.c_str(), s.pass.c_str());
}

void loop() {
  if (s_provisioning) {
    ledTick();
    g_console.poll();
    if (millis() - s_provStarted > PROV_TIMEOUT_MS) {
      LOGF("Improv window expired; rebooting to retry");
      ESP.restart();
    }
    return;   // engine stays offline for the whole provisioning boot
  }

  // Network health first: a dead STA means a dead web shell + no HA later.
  wifiWatchdog();

  // Engine: run any queued background scan (console 'scan').
  g_mesh.loop();

  // HA bridge: refresh the retained hub-state JSON (~30 s cadence).
  g_bridge.loop();

  // Serial console.
  g_console.poll();

  ledTick();
}
