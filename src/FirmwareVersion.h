#ifndef FIRMWARE_VERSION_H
#define FIRMWARE_VERSION_H

// ============================================================================
//  FirmwareVersion.h — the SINGLE SOURCE OF TRUTH for the firmware version.
//
//  Every surface that reports a version reads THIS (lora-relay convention):
//    - the boot banner (main.cpp)
//    - Improv provisioning device info (main.cpp)
//    - /api/status  "fw" (web/WebServer.cpp)
//    - Phase 2: tools/build_spa.py bakes it into the SPA for the mismatch
//      check (same regex as lora-relay)
//
//  Rules:
//    - Bump FIRMWARE_VERSION HERE when a phase ships.
//    - Only the CURRENT release is described below (one short block).
//      Do NOT accumulate a changelog here.
//    - Commit once per phase; the version string is the contract.
// ============================================================================

#define FIRMWARE_VERSION  "1.1.0"

#ifndef kBoardModel
#define kBoardModel  "Waveshare ESP32-S3 Zero (ESP32-S3)"
#endif

// --- Current release --------------------------------------------------------
// 1.1.0 — Phase 1, connectivity foundation: WiFi STA + Improv provisioning,
//         mDNS + esp_http_server shell, /api/status + /api/lights, WS /ws/log,
//         serial config commands, LittleFS ready for the Phase-2 SPA.
#endif // FIRMWARE_VERSION_H
