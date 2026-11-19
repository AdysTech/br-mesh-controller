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
// How long (seconds) the initial BLE scan runs when hunting for factory-key
// bulbs that still need keying.
#define SCAN_DURATION    5

// --- WiFi / provisioning timing (bridge mode) ---------------------------------
#define WIFI_CONNECT_TIMEOUT_MS  15000UL   // bounded STA attempt at boot
#define PROV_TIMEOUT_MS          120000UL  // Improv window per boot, then reboot
