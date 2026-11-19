#pragma once
// ============================================================================
//  WebServer — the esp_http_server shell (Phase 1: Connectivity Foundation).
//
//  Routes (specific before the SPA catch-all):
//    GET  /            -> gzipped /index.html.gz from LittleFS (Phase 2 SPA),
//                         or the embedded fallback status page (Phase 1)
//    GET  /*           -> same handler (SPA catch-all, registered last)
//    GET  /api/status  -> device + WiFi + engine status (JSON)
//    GET  /api/lights  -> registered lights (JSON)
//    WS   /ws/log      -> live log stream: log-tail greeting + broadcast
//
//  WS dispatch model (verified on this core via lora-relay):
//  esp_http_server invokes the WS handler when the CLIENT SENDS A FRAME, not
//  at handshake. So the client must send a hello frame; the handler consumes
//  that frame (2-phase httpd_ws_recv_frame), replies (greeting) with the
//  synchronous httpd_ws_send_frame, and the registry is idempotent.
// ============================================================================
#include <esp_http_server.h>

/// Live WebSocket /ws/log client registry (connection ids, lazily pruned
/// when a broadcast to one fails).
#define WEB_MAX_WS_CLIENTS 8

/// Start LittleFS + the httpd server + route table. True on success.
/// Safe to call once; returns true if already running.
bool webServerInit();
/// Stop the server. Safe when not started.
void webServerShutdown();
bool webServerIsRunning();

/// The active server handle (nullptr when down). Exposed per BRIDGE_PLAN P1.1.
extern httpd_handle_t g_webServer;
