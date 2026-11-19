#pragma once
// ============================================================================
//  Log ring buffer + WebSocket broadcast (the web console viewer's backend).
//
//  Every firmware log line goes through blog()/logLine(): it is printed to
//  Serial (as before) AND pushed into a small ring buffer. When the web
//  server is up, new lines are also broadcast to /ws/log WebSocket clients.
// ============================================================================
#include <Arduino.h>
#include <string>

/// Format a log line (printf-style), print to Serial, push to the ring,
/// and broadcast to web-console clients. Use LOGF(...) in code.
void blog(const char *fmt, ...);

/// Push a ready-made line (no formatting). Also prints to Serial.
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

#define LOGF(fmt, ...) blog(fmt, ##__VA_ARGS__)
#define LOG(x)         blog("%s", (const char*)(x))
