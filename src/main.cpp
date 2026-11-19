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
//  provisioning fallback. Web (Phase 1) + MQTT/HA (Phase 3) front ends land
//  here later.
//
//  Provisioning rule: on a provisioning boot the mesh engine is NEVER started
//  — Improv owns the BLE radio for that boot, and provisioning always ends in
//  a reboot, so Improv and MeshController never coexist (and never share a
//  boot). A pre-provisioned board skips Improv and reaches the engine.
// ============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <Network.h>
#include <ImprovWiFiBLE.h>

#include "config.h"
#include "MeshController.h"
#include "console.h"
#include "settings.h"
#include "statusled.h"

// ============================================================================
//  App instances: one engine + its serial driver + Improv (provisioning).
// ============================================================================
MeshController g_mesh;
Console g_console(g_mesh);
ImprovWiFiBLE g_improv;

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
  Serial.println("prov: clearing saved WiFi creds, rebooting into Improv");
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
    Serial.printf("hostname '%s' saved + applied (DHCP lease picks it up at renewal/reconnect)\n", name);
    return;
  }
  Serial.printf("hostname '%s' saved — active from the next boot/connect\n", name);
}

// Start the Improv BLE service. The brmesh NVS settings are the single
// source of truth for creds: a custom connect handler saves them (bypassing
// the library's own driver-NVS connect path) and reboots, so the very next
// boot goes straight at STA with the new creds.
void launchProvisioning() {
  s_provisioning = true;
  s_provStarted  = millis();
  ledSet(LED_AP);
  Serial.println("=== Improv provisioning (BLE) ===");
  Serial.printf("  BLE name : %s\n", improvName());
  Serial.println("  Phone/PC : open https://www.improv-wifi.com (Chrome/Edge) and scan");
  Serial.printf("  Window   : %lu s, then reboot to retry\n", PROV_TIMEOUT_MS / 1000);

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);    // wipe any stale driver-NVS creds

  g_improv.setCustomConnectWiFi([](const char* ssid, const char* pass) -> bool {
    Serial.printf("Improv: creds received (ssid=%s) — saving to NVS\n", ssid);
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
                         improvName(), "1.0", improvName());
  Serial.println("Improv: ready. Serial console still available (type 'help').");
}

// ============================================================================
//  Setup / loop
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(250);
  Serial.println("\n=== BRMesh Controller ===");

  ledInit();
  ledSet(LED_BOOT);

  // ---- BRIDGE_PLAN §5 boot flow: NVS creds -> bounded STA -> Improv -------
  // The mesh engine is deliberately NOT started on a provisioning boot (see
  // the file header): Improv owns the BLE radio for that boot.
  const Settings s = loadSettings();
  bool wifiUp = false;
  if (!s.ssid.empty()) {
    WiFi.persistent(false);   // the driver's own NVS STA slot must not win
    // ORDER MATTERS (core quirk, verified on hardware): the core snapshots
    // the stored name onto the STA netif INSIDE WiFi.mode() —
    // WiFiGenericClass::mode() -> esp_netif_set_hostname(getHostname()).
    // WiFi.setHostname() after mode() only writes a static buffer that is
    // never applied again (symptom: DHCP advertised default esp32s3-<mac>).
    WiFi.setHostname(s.name.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.begin(s.ssid.c_str(), s.pass.c_str());
    Serial.printf("WiFi: connecting to '%s' (timeout %lu s)...\n",
                  s.ssid.c_str(), WIFI_CONNECT_TIMEOUT_MS / 1000);
    const uint32_t t0 = millis();
    while (millis() - t0 < WIFI_CONNECT_TIMEOUT_MS) {
      if (WiFi.status() == WL_CONNECTED) break;
      delay(250);
    }
    wifiUp = (WiFi.status() == WL_CONNECTED);
    if (wifiUp) {
      Serial.printf("WiFi: connected, IP %s\n", WiFi.localIP().toString().c_str());
      ledSet(LED_WIFI);
    }
  }
  if (!wifiUp) {
    Serial.println("WiFi: not configured or connect failed -> provisioning");
    launchProvisioning();
    g_console.banner();
    return;   // loop(): console + LED + window timeout only
  }

  // 1) Engine: BLE init + restore/generate the mesh key.
  g_mesh.begin();
  {
    const uint8_t* k = g_mesh.key();
    Serial.printf("Mesh key: %02X %02X %02X %02X\n", k[0], k[1], k[2], k[3]);
  }

  // 2) Restore previously-registered lights (no re-keying).
  g_mesh.loadLights();

  Serial.println("Ready. Use the serial console for discovery (type 'help').");
  Serial.println("To key the bulb, power-cycle it, then type 'bootcatch 12'.");
  g_console.banner();
}

void loop() {
  if (s_provisioning) {
    ledTick();
    g_console.poll();
    if (millis() - s_provStarted > PROV_TIMEOUT_MS) {
      Serial.println("Improv window expired; rebooting to retry");
      ESP.restart();
    }
    return;   // engine stays offline for the whole provisioning boot
  }

  // Engine: run any queued background scan (console 'scan' / 'bootcatch').
  g_mesh.loop();

  // Serial console.
  g_console.poll();

  ledTick();
}
