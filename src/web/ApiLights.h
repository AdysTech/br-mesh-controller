#pragma once
// ============================================================================
//  API routes — scan + per-light control (BRIDGE_PLAN.md §6).
//
//  esp_http_server has no path parameters, so the per-light address is a
//  query parameter: /api/light?n=1  (the plan's /api/light/{n}).
//
//    POST   /api/scan           {"seconds":N}  discovery + key new bulbs
//    POST   /api/scan/stop      end a running scan/capture early
//    POST   /api/light?n=N      {"on":bool} | {"brightness":0-127} |
//                               {"rgb":[r,g,b]} | {"ct":N}
//    PATCH  /api/light?n=N      {"name":"..."}  rename (FR-13)
//    DELETE /api/light?n=N      remove from list + NVS (FR-12; bulb key kept)
// ============================================================================
#include <esp_http_server.h>

bool registerLightRoutes(httpd_handle_t srv);
