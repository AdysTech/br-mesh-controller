#pragma once
// ============================================================================
//  API routes — config, provisioning, factory reset, raw capture
//  (BRIDGE_PLAN.md §6).
//
//    GET  /api/config          NVS settings, passwords masked
//    POST /api/config          save settings (name/ssid/pass/mqtt…);
//                              name applies live, WiFi changes need a reboot
//    POST /api/provision       clear saved WiFi -> reboot into Improv
//    POST /api/factory-reset   wipe NVS 'brmesh' (key + light list) -> reboot
//    POST /api/raw             {"sec":N,"wake":bool} raw FastCon capture
// ============================================================================
#include <esp_http_server.h>

bool registerSystemRoutes(httpd_handle_t srv);
