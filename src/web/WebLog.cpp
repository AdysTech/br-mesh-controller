// Log ring buffer + broadcast (see WebLog.h).
#include "WebLog.h"
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include <cstdio>
#include <cstring>
#include <cstdarg>

#define LOG_RING 128    // entries kept
#define LOG_LEN  96     // max chars per entry

static char ring[LOG_RING][LOG_LEN];
static int ringHead = 0;      // next write slot
static int ringCount = 0;
static portMUX_TYPE ringMux = portMUX_INITIALIZER_UNLOCKED;

// Defined in WebServer.cpp (single build — the web stack always links).
extern bool webWsHasLogClients(void);
void wsLogBroadcast(const char *msg);

static void pushLine(const char *line, size_t n) {
  if (n >= LOG_LEN) n = LOG_LEN - 1;
  taskENTER_CRITICAL(&ringMux);
  memcpy(ring[ringHead], line, n);
  ring[ringHead][n] = 0;
  ringHead = (ringHead + 1) % LOG_RING;
  if (ringCount < LOG_RING) ringCount++;
  taskEXIT_CRITICAL(&ringMux);
}

static void tailLine(int age, char *out) {
  // age 0 = newest
  int idx = (ringHead - 1 - age + LOG_RING * 4) % LOG_RING;
  taskENTER_CRITICAL(&ringMux);
  memcpy(out, ring[idx], LOG_LEN);
  taskEXIT_CRITICAL(&ringMux);
}

void logLine(const std::string &line) {
  Serial.println(line.c_str());
  pushLine(line.c_str(), line.size());
  if (webWsHasLogClients()) wsLogBroadcast(line.c_str());
}

void blog(const char *fmt, ...) {
  char buf[192];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n < 0) n = 0;
  if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
  buf[n] = 0;

  Serial.println(buf);
  pushLine(buf, n);
  if (webWsHasLogClients()) wsLogBroadcast(buf);
}

std::string logTail(int n) {
  int count, head;
  taskENTER_CRITICAL(&ringMux);
  count = ringCount;
  head = ringHead;
  taskEXIT_CRITICAL(&ringMux);

  int take = n < count ? n : count;
  std::string out;
  for (int i = take - 1; i >= 0; i--) {   // oldest first
    char tmp[LOG_LEN];
    tailLine(i, tmp);
    if (out.size()) out += "\n";
    out += tmp;
  }
  return out;
}
