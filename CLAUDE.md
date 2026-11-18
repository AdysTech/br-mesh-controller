# CLAUDE.md — BRMesh → Home Assistant controller (ESP32-S3)

ESP32-S3 firmware that bridges **BRGD/BRMesh BLE lights** into **Home Assistant**
over MQTT. Currently **POC mode** (`ENABLE_WIFI=0` in [src/config.h](src/config.h)):
BLE + serial console only, no WiFi/MQTT/HA.

**Read the siblings before touching code — they are the sources of truth:**
- [protocol.md](protocol.md) — the FastCon air protocol, byte-level (framing,
  command types, control payloads, factory-identity frame), how it was captured,
  and the two community references it builds on.
- [BRIDGE_PLAN.md](BRIDGE_PLAN.md) — the forward build (web UI, MQTT/HA, phases).

This file keeps only the model, current status, the catch recipe, console, and
toolchain.

**Code layout — a 3-layer split (keep `main.cpp` lean for the bridge stage):**
- [src/MeshController.{h,cpp}](src/MeshController.h) — the engine: key + lights + BLE advertise/scan + discovery/keying + NVS persistence
- [src/console.{h,cpp}](src/console.h) — the serial console driver (POC UI); consumes a `MeshController&`
- [src/main.cpp](src/main.cpp) — thin app shell: setup/loop + the HA/HTTP front ends under `ENABLE_WIFI`
- [src/protocol.{h,cpp}](src/protocol.h) — FastCon byte codec (unchanged) · [src/light.h](src/light.h) — shared `LightDevice`

---

## 1. Core model (do NOT deviate)

- These bulbs are **NOT real BLE Mesh**. FastCon is BroadLink's patented
  **broadcast** air interface: no pairing, no connection, no acknowledgements.
  Commands travel as BLE advertising frames the bulbs simply listen to.
- The ESP is the **KEY ORIGINATOR**: it generates its own 4-byte key and writes
  it onto factory-keyed bulbs (SET_KEY). **We deliberately do NOT extract the key
  from the Android app** — that is the point of this project.
- **Factory key** (what fresh bulbs ship with) = `5E 36 7B C4`, riding in the
  clear in the factory-identity advertisement (protocol.md §2.6) — that is what
  makes SET_KEY possible.
- Expose **each light individually** as an HA entity. No group APIs.
- **Persistent boot:** key + light list in NVS (`brmesh` namespace); on boot,
  restore them (no re-keying) and key **only new** factory-keyed bulbs.

## 2. Proven state → the standing rules it gave us

The POC is **proven** on `70:e7:f2:72:70:e8` (light #1, key `09 90 1D D8`):
detect → key → control → NVS persistence across reboots. Don't re-derive it.

- **[src/protocol.cpp](src/protocol.cpp) is byte-identical to the proven community
  reference** (protocol.md §3). It is the wire source of truth — do NOT "fix" it.
- **NVS write paths must call `prefs.begin(name, false)`.** `readOnly=true` opens the
  namespace read-only and `put*` silently no-ops (key-gen + `saveLights()` in
  [src/MeshController.cpp](src/MeshController.cpp)).
- **NimBLE can't advertise and scan at once** — the controller drops the advertiser
  the moment a scan starts. WAKE+scan must be time-sliced via
  [slicedAdvertiseScan()](src/MeshController.cpp) (advertise burst → scan burst loop);
  never the concurrent Bluedroid pattern.
- **Bulbs on air:** factory OUI `f2:72:70:e8`, type `2A A8`. Test target = `70:e7`
  (on the Tasmota power switch). `2a:e6` / `0b:ed` are **not** test targets — ignore.
- **Next:** the bridge per [BRIDGE_PLAN.md](BRIDGE_PLAN.md).

## 3. Discovery / keying (the proven catch recipe)

The factory-identity frame is only emitted in the seconds after power-up, and
only while WAKE is on the air. Proven recipe:

1. `scan 12` (duration configurable)
2. wait ~1 s → Tasmota `Power OFF`
3. wait ~2 s → `Power ON` — bulb boots *into* the ongoing WAKE with the full
   window left to advertise

Driver: [tools/scan_catch.py](tools/scan_catch.py). **NimBLE can't advertise and
scan at once** (the ESP32 controller drops the advertiser when the scan starts),
so `scan` uses [slicedAdvertiseScan()](src/MeshController.cpp) — a WAKE/SET_KEY advertise
burst → scan burst loop — instead of the old concurrent Bluedroid pattern. `wantDuplicates=true`
still matters: the bulb interleaves bare beacons with identity frames and dedup
drops them. Keying (`addLight`) time-slices the same way. If the NVS light list is
ever lost: `readd <mac> [num]`.

## 4. Serial console (POC, 115200 baud)

`help` for the full list. Essentials:

```
list                  known lights            key                   current mesh key
sel <n>               target light            on/off/br/ct/rgb ... control (see protocol.md §2.5)
readd <mac> [num]     re-register a keyed bulb (no air traffic — list recovery)
scan [sec=5]          discovery + key new bulbs (use 12+ with a mid-scan power-cycle)
bootcatch [sec=12]    WAKE + concurrent scan (run while power-cycling a bulb)
raw [sec]             unfiltered per-frame hex dump (debug)
rawwake [sec]         WAKE broadcast + every frame (proved WAKE triggers identity)
wake [sec] | wakephone [sec]                  broadcast-only debug
```

## 5. Build / flash / serial toolchain

- **Only working pio/python:** `/home/rao/.platformio/penv/bin/pio` and
  `/home/rao/.platformio/penv/bin/python`.
- Board `waveshare_esp32_s3_zero` (pioarduino core, 4 MB flash / 2 MB PSRAM),
  native USB: `-DARDUINO_USB_MODE=1 -DARDUINO_USB_CDC_ON_BOOT=1`.
- **POC build boundary:** `build_src_filter` in [platformio.ini](platformio.ini)
  scopes the build to `main.cpp` + `protocol.cpp` + `MeshController.cpp` +
  `console.cpp` (+ the shared `light.h`); the untracked bridge scaffolding
  (`settings.*`, `statusled.*`, `web/`) must NOT compile into the POC.
- **Serial port:** `/dev/ttyACM0` or `/dev/ttyACM1` — **it can swap on
  power-cycles** (`ls /dev/ttyACM*`; override with `ESP_SERIAL_PORT=...`).
  `monitor_port` in platformio.ini is stale.
- **DTR/RTS from the host does NOT reset this board.**
  Reset via the RESET button or a USB power-cycle; `tools/esp_serial.py boot`
  only listens.

```bash
P=/home/rao/.platformio/penv/bin
$P/pio run -e waveshare_esp32_s3_zero --target upload
$P/python tools/esp_serial.py boot 12       # listen — press RESET within the window
$P/python tools/esp_serial.py cmd "on 1" 4
```

**Power toggle for `70:e7…`:** Tasmota via `TASMOTA_URL` / `curl`
(`?cmnd=Power%20OFF|ON`) — env-driven, no LAN IP in the repo. **One request per
state change, never loop/retry** — the sandbox classifier flags repeated network
calls; on a denial, **stop and report**, don't retry verbatim.

## 6. Tools (tools/, all pyserial + env-driven)

- [tools/esp_serial.py](tools/esp_serial.py) — plumbing: `boot [sec]`, `cmd <text> [sec]`.
- [tools/scan_catch.py](tools/scan_catch.py) — **the proven one**: `scan 12` + early power-cycle.
- [tools/raw_catch.py](tools/raw_catch.py), [tools/rawwake_catch.py](tools/rawwake_catch.py) —
  raw manufacturer-frame dumps; the field path for reverse-engineering a new bulb model.
- [tools/staged_capture.py](tools/staged_capture.py) — ENTER-driven staged passive capture
  (hunt driver for the phone's one-shot group frames; `scanstop` ends a stage).
  Capture logs land in `tools/capture_*.log`.
- [tools/decode_fastcon.py](tools/decode_fastcon.py) — **the FastCon decoder**: inverts
  protocol.cpp's encode pipeline (preamble restore, LFSR de-whiten seed `0x25`), reads
  the command TYPE off a captured payload without needing the command key.

## 7. Standing rules / gotchas

- **Trust this file as the repo map.** If a tool isn't listed in §6 it doesn't exist —
  do NOT re-scan the repo for prior art or re-derive the codec/whitening that
  `protocol.md` + [tools/decode_fastcon.py](tools/decode_fastcon.py) already define.
  Start from the map, not from a search. Keep §6 current when you add tooling.
- **DO NOT** loop/retry network calls or pin toggles — stop and report on denial.
- The `#if ENABLE_WIFI` blocks (HA/MQTT/HTTP) are **dead code in the POC** but must
  keep compiling when flipped to 1 — the HA lib (`dawidchyrzynski/home-assistant-
  integration`) is commented out of [platformio.ini](platformio.ini) and re-added then.
- `protocol.md` is the wire-format source of truth — don't copy frame layout back
  here.
