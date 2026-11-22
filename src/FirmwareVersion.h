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

#define FIRMWARE_VERSION  "1.2.2"

#ifndef kBoardModel
#define kBoardModel  "Waveshare ESP32-S3 Zero (ESP32-S3)"
#endif

// --- Current release --------------------------------------------------------
// 1.2.2 — Colour-temp is Kelvin end-to-end: `ct` is a Kelvin label (2700 warm
//         → 6000 cool) across the API, console, and believed_state; it is
//         split into the WW/CW crossfade (d[6] warm, d[7] cool, d[6]+d[7]=255)
//         only at TX. A legacy NVS `ct` is simply superseded by the next CT
//         op — no migration.
#endif // FIRMWARE_VERSION_H
