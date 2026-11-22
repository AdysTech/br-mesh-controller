#pragma once
// ============================================================================
//  Leveled log ring buffer + WebSocket broadcast (the web console backend).
//
//  P2.11 — every firmware log line goes through a level-gated sink (blogE /
//  blog / blogD): it is time-stamped ("t=<uptime>s "), printed to Serial,
//  pushed into a small ring buffer, and — when the web server is up —
//  broadcast to /ws/log clients. The level is settable at runtime
//  (console `loglevel`) and filters out the noise you don't need:
//
//    0 = quiet    — errors only            (LOGE)
//    1 = normal   — operational events     (blog / LOGF)  [default]
//    2 = verbose  — per-frame / debug      (blogD / LOGD)
//
//  Modeled on lora-relay's Shared.h logger (g_logLevel + level-gated macros +
//  uptime prefix), with the second sink being the web ring instead of BLE.
// ============================================================================
#include <Arduino.h>
#include <string>

// Compile-time default log level (see the level table above). Override at
// build time with -DLOG_LEVEL=n if you want a different default.
#ifndef LOG_LEVEL
#define LOG_LEVEL 1
#endif

/// Runtime log level (0=quiet, 1=normal, 2=verbose). Set from the console
/// (`loglevel`) or a future API. NOT persisted — resets to LOG_LEVEL on reboot.
extern volatile int8_t g_logLevel;

/// Error  — always emitted (level >= 0). Failures, unrecoverable problems.
void blogE(const char *fmt, ...);
/// Info   — operational events (level >= 1). The default working level.
void blog(const char *fmt, ...);
/// Debug  — per-frame / trace (level >= 2). Only when explicitly enabled.
void blogD(const char *fmt, ...);

/// Push a ready-made line (no formatting) at the info level. Also printed.
void logLine(const std::string &line);

/// Last `n` ring entries, concatenated with '\n' (for the WS greeting).
std::string logTail(int n);

// --- broadcast plumbing (implemented in WebServer.cpp) ---------------------
/// True when at least one /ws/log client is connected (cheap; check before
/// building a broadcast payload).
bool webWsHasLogClients(void);
/// Push a line to all /ws/log clients as {"type":"log","text":...}.
/// No-op when no clients are connected.
void wsLogBroadcast(const char *msg);

// --- ergonomic macros (format-checked by the compiler) --------------------
#define LOGE(fmt, ...) blogE(fmt, ##__VA_ARGS__)
#define LOGF(fmt, ...) blog(fmt,  ##__VA_ARGS__)
#define LOGD(fmt, ...) blogD(fmt, ##__VA_ARGS__)
#define LOG(x)         blog("%s", (const char*)(x))
