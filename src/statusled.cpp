// WS2812B status LED driver (see statusled.h).
//
// Waveshare ESP32-S3-Zero: 1x WS2812B, GPIO 21 — the board variant already
// provides WS_RGB / RGB_BUILTIN, so there is no custom pin define here.
//
// Driven by the arduino-esp32 core's native RMT API — no third-party
// NeoPixel library.
//
// Colour order: this is a standard WS2812B, i.e. physically GRB. The board
// variant claims RGB_BUILTIN_LED_COLOR_ORDER=RGB, which would swap R and G,
// so we pass the standard GRB order explicitly and keep plain R/G/B values in
// the state table below.
#include "statusled.h"

#ifdef RGB_BUILTIN
static void ledWrite(uint8_t r, uint8_t g, uint8_t b) {
  rgbLedWriteOrdered(RGB_BUILTIN, LED_COLOR_ORDER_GRB, r, g, b);
}
#else
static inline void ledWrite(uint8_t r, uint8_t g, uint8_t b) { (void)r; (void)g; (void)b; }
#endif

static LedState g_state = LED_BOOT;

void ledInit() {
  ledWrite(0, 0, 0);   // latch off; also brings up the RMT channel
}

void ledSet(LedState s) { g_state = s; }

static void colorFor(LedState s, uint32_t now, uint8_t& r, uint8_t& g, uint8_t& b) {
  r = g = b = 0;
  switch (s) {
    case LED_BOOT: if ((now / 250) % 2) { r = 110; g = 110; b = 110; } break;
    case LED_SCAN: if ((now / 160) % 2) { r = 0;   g = 90;  b = 255; } break;
    case LED_KEY:  if ((now / 240) % 2) { r = 255; g = 140; b = 0;   } break;
    case LED_AP:   if ((now / 300) % 2) { r = 255; g = 40;  b = 40;  } break;
    case LED_ERR:  if ((now / 90)  % 2) { r = 255; g = 0;   b = 0;   } break;
    case LED_WIFI: r = 0; g = 90; b = 40; break;
    case LED_IDLE:
    default:       r = 0; g = 45; b = 0; break;
  }
}

void ledTick() {
  static uint32_t last = 0;
  uint32_t now = millis();
  if (now - last < 100) return;
  last = now;
  uint8_t r, g, b;
  colorFor(g_state, now, r, g, b);
  ledWrite(r, g, b);
}
