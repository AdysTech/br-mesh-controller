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
//    - Bump FIRMWARE_VERSION HERE after EVERY source edit (e.g. 1.3.0 ->
//      1.3.1) so the board's running build is always identifiable via
//      /api/status `fw` without asking which one it was flashed with.
//    - Only the CURRENT release is described below (one short block).
//      Do NOT accumulate a changelog here.
//    - Commit once per phase; the version string is the contract.
// ============================================================================

#define FIRMWARE_VERSION  "1.4.4"

#ifndef kBoardModel
#define kBoardModel  "Waveshare ESP32-S3 Zero (ESP32-S3)"
#endif

// --- Current release --------------------------------------------------------
// 1.4.4 — HA `schema:"json"` light state now matches the format HA actually
//         uses (verified against the broker's own /set command): uppercase
//         "ON"/"OFF", and the RGB colour as the object {"r","g","b"} (not an
//         array) — the two mismatches that left the entity "Unknown" (no working
//         toggle/slider/colour). The colour is also parsed from the object form
//         HA sends to /set, so incoming RGB commands are no longer dropped.
//         (1.4.3: live state re-assert + entity name; 1.4.2: full RGB+CT light,
//         schema:"json", mireds 166–370.)
#endif // FIRMWARE_VERSION_H
