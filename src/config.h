#pragma once
// ============================================================================
//  BRMesh Controller — build configuration
// ============================================================================

// --- WiFi (the network your Home Assistant / MQTT broker is on) -------------
#define WIFI_SSID     "YOUR_SSID"
#define WIFI_PASS     ""
// IPAddress lives in the network core — only use in main.cpp (where the
// network headers are already included).
#define MQTT_BROKER_ADDR IPAddress(192, 168, 0, 1)
#define MQTT_PORT     1883

// --- On-board status LED ----------------------------------------------------
// ESP32-S3 devkitc-1 typically drives the LED on GPIO 8.
// Set to -1 if your board has no LED / you don't want to touch one.
#define LED_PIN       8

// --- Discovery --------------------------------------------------------------
// How long (seconds) the initial BLE scan runs when hunting for factory-key
// bulbs that still need to be keyed.
#define SCAN_DURATION 5

// --- Mode -------------------------------------------------------------------
// 0 = BLE-only TEST mode: no WiFi, no MQTT, no HA. Serial console drives
//     discovery + keying + control (used to prove the POC).
// 1 = full bridge: WiFi (or AP fallback) + web UI + MQTT/HA + captive setup.
#define ENABLE_WIFI   0
