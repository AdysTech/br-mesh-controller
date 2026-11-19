#pragma once
// ============================================================================
//  WS2812B status LED (waveshare_esp32_s3_zero, GPIO 21, single pixel).
//
//  States:
//   BOOT  - white blink        (setup)
//   SCAN  - blue blink         (discovery scan running)
//   KEY   - amber blink        (writing our key onto a bulb)
//   WIFI  - solid green        (connected to WiFi, ready)
//   AP    - red blink          (provisioning — Improv is live)
//   IDLE  - dim green          (ready, nothing in flight)
//   ERR   - fast red blink     (fatal-ish condition)
// ============================================================================
#include <Arduino.h>

enum LedState { LED_BOOT, LED_IDLE, LED_WIFI, LED_SCAN, LED_KEY, LED_AP, LED_ERR };

void ledInit();
void ledSet(LedState s);
/// Call from loop(): advances blink/phase timing.
void ledTick();
