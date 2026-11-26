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

#define FIRMWARE_VERSION  "1.5.4"

#ifndef kBoardModel
#define kBoardModel  "Waveshare ESP32-S3 Zero (ESP32-S3)"
#endif

// --- Current release --------------------------------------------------------
// 1.5.4 — the HA "Scan" sensor now tracks the scan LIVE. A discovery/keying
//         scan blocks the app loop for its whole window, so the periodic hub-
//         state publish (every 30 s) could never land a "scanning" state — the
//         sensor sat at "idle" through a live scan. MeshController now fires a
//         scan-state callback at start and finish, and the bridge re-publishes
//         the hub state at that transition, so HA flips to "scanning" the moment
//         a scan begins (from HA, the SPA, or the console) and back to "idle"
//         when it ends. (1.5.3: the REST API response builders emit cJSON trees
//         (apiSendCJson) and the SPA Discovery section is a single "Scan now"
//         button using the configured window — the same duration the HA "Start
//         scan" button uses; 1.5.2: the discovery window is a single operator-
//         configured NVS "scandur" value shared by every scan path, defaulting
//         to 30 s; 1.5.1: soft "name already used by bulb #N" hint on the
//         Lights rename field; 1.5.0: full cJSON JSON layer; 1.4.4: HA
//         schema:"json" state; 1.4.3: live state re-assert; 1.4.2: full RGB+CT
//         light.)
#endif // FIRMWARE_VERSION_H
