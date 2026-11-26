#pragma once
// ============================================================================
//  BRMesh Controller — build configuration
// ============================================================================

// --- Firmware version (single source of truth: FirmwareVersion.h) -----------
// Bump FIRMWARE_VERSION / kBoardModel THERE per phase — see that file's rules.
// Reported by /api/status, the boot banner, and Improv device info.
#include "FirmwareVersion.h"

// --- Defaults (NVS seed values; saved settings win — see settings.*) --------
#define DEFAULT_NAME      "brmesh"
#define DEFAULT_WIFI_SSID ""        // empty = not configured yet -> Improv at boot
#define DEFAULT_WIFI_PASS ""
#define DEFAULT_MQTT_HOST "192.168.0.1"
#define DEFAULT_MQTT_PORT 1883

// --- On-board LED -------------------------------------------------------------
// WS2812B on GPIO 21: the board variant defines it (WS_RGB / RGB_BUILTIN),
// see statusled.* — no pin define here.

// --- Discovery -----------------------------------------------------------------
// Default discovery window (seconds) — how long the WAKE+scan runs when hunting
// for factory-keyed bulbs that need keying. This is the compile-time seed; the
// operator-set NVS value ("scandur", settings.scanSec) wins and is what the HA
// scan button, the SPA scan, and the console `scan` all default to. Long enough
// to reach the power switch and cycle a fresh bulb (the pairing window only
// opens in the seconds after power-up).
#define SCAN_DURATION    30

// --- Command queue (Phase 3, BRIDGE_PLAN §8) ------------------------------------
// Every control (MQTT set / web / serial) ENQUEUES a light; the pacer in
// MeshController::loop() is the sole air-time owner. T_TX_MS = minimum gap
// between broadcasts; CMD_REPEAT = how many times one command goes on the air.
#define CMD_QUEUE_SIZE   64
#define T_TX_MS          150
#define CMD_REPEAT       2

// --- MQTT reconnect backoff (Phase 3) -------------------------------------------
// esp-mqtt retries a failed connect every network.reconnect_timeout_ms (10 s)
// forever. After MQTT_FAST_RETRIES consecutive failures the bridge stops the
// client (log quiets) and restarts it from HaBridge::loop() after the cooldown.
#define MQTT_FAST_RETRIES          5
#define MQTT_RECONNECT_COOLDOWN_MS 300000UL   // 5 min

// --- WiFi / provisioning timing (bridge mode) ---------------------------------
#define WIFI_CONNECT_TIMEOUT_MS  15000UL   // bounded STA attempt at boot
#define PROV_TIMEOUT_MS          120000UL  // Improv window per boot, then reboot
