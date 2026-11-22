// Leveled log ring buffer + broadcast (see WebLog.h). P2.11.
//
// Every line is level-gated on g_logLevel, then time-stamped ("t=<uptime>s "),
// printed to Serial, pushed into the ring, and broadcast to /ws/log clients.
// The gate happens before any formatting so a suppressed line costs nothing.
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

// P2.11 — runtime log level (0=quiet, 1=normal, 2=verbose). NOT persisted;
// resets to the compile-time LOG_LEVEL on reboot.
volatile int8_t g_logLevel = LOG_LEVEL;

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

// Shared sink: Serial + ring + web broadcast (no level gate — callers gate).
static void emitLine(const char *line, size_t n) {
  Serial.println(line);
  pushLine(line, n);
  if (webWsHasLogClients()) wsLogBroadcast(line);
}

// Level-gated, time-stamped printf sink. `level` is the MINIMUM level at
// which the line prints: 0=error (always), 1=info, 2=debug.
static void vlog(int level, const char *fmt, va_list ap) {
  if ((int)g_logLevel < level) return;            // gate first — zero cost when off
  char body[176];
  int n = vsnprintf(body, sizeof(body), fmt, ap);
  if (n < 0) n = 0;
  if (n >= (int)sizeof(body)) n = (int)sizeof(body) - 1;
  body[n] = 0;

  char line[192];
  int len = snprintf(line, sizeof(line), "t=%lus ", (unsigned long)(millis() / 1000UL));
  if (len < 0) len = 0;
  snprintf(line + len, sizeof(line) - len, "%s", body);
  emitLine(line, strlen(line));
}

void blogE(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vlog(0, fmt, ap); va_end(ap); }
void blog(const char *fmt, ...)  { va_list ap; va_start(ap, fmt); vlog(1, fmt, ap); va_end(ap); }
void blogD(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vlog(2, fmt, ap); va_end(ap); }

// A ready-made line (no format specifiers), time-stamped at the info level.
void logLine(const std::string &line) {
  char out[192];
  int len = snprintf(out, sizeof(out), "t=%lus %.*s",
                     (unsigned long)(millis() / 1000UL), (int)line.size(), line.c_str());
  emitLine(out, len < 0 ? 0 : (size_t)len);
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
