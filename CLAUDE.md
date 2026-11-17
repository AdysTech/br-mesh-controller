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

## 2. STATUS (2026-11-18)

**POC COMPLETE** on `70:e7:f2:72:70:e8` (light #1, our key `09 90 1D D8`):
detect → key → control (physically verified) → NVS persistence across reboots,
plus `readd` list recovery. **Do not re-litigate:** [src/protocol.cpp](src/protocol.cpp)
is byte-identical to the proven community reference (protocol.md §3), the active
scan sees the bulbs, and the detection/keying logic matches the real captured
factory frame.

**Fixed gotcha:** `prefs.begin(name, readOnly=true)` opens NVS read-only, so
`put*` silently no-ops — write paths must use `false`
([src/main.cpp:585](src/main.cpp#L585) key-gen, [src/main.cpp:594](src/main.cpp#L594)
`saveLights()`).

**BLE stack:** glue in [src/main.cpp](src/main.cpp) is **NimBLE-Arduino 2.x**
(migrated 2026-11-16 from Bluedroid so later Improv provisioning shares one host
stack). `protocol.cpp` untouched. **NimBLE-esp32 drops the advertiser the moment
a scan starts**, so WAKE+scan can't run concurrently (Bluedroid could). The fix is
[slicedAdvertiseScan()](src/main.cpp) — a WAKE/SET_KEY advertise burst → scan burst
loop — that `rawWake`, `scan`, and `addLight` all route through. Re-proven on the
NimBLE build 2026-11-18 (detect → key → ON/OFF all verified).

**The 3 bulbs** (all factory OUI `f2:72:70:e8`, type `2A A8`):

| MAC | state |
|---|---|
| `70:e7:f2:72:70:e8` | **keyed by us (#1)**; on the Tasmota power switch — the only bulb we can power-cycle |
| `2a:e6:f2:72:70:e8` | untouched, no controllable power |
| `0b:ed:f2:72:70:e8` | untouched, no controllable power |

**Remaining:** key the other two (put on controllable power or factory-reset via
the app, then run the §3 recipe); then the bridge per BRIDGE_PLAN.md.

## 3. Discovery / keying (the proven catch recipe)

The factory-identity frame is only emitted in the seconds after power-up, and
only while WAKE is on the air. Proven recipe:

1. `scan 12` (duration configurable)
2. wait ~1 s → Tasmota `Power OFF`
3. wait ~2 s → `Power ON` — bulb boots *into* the ongoing WAKE with the full
   window left to advertise

Driver: [tools/scan_catch.py](tools/scan_catch.py). **NimBLE can't advertise and
scan at once** (the ESP32 controller drops the advertiser when the scan starts),
so `scan` uses [slicedAdvertiseScan()](src/main.cpp) — a WAKE/SET_KEY advertise
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
  scopes the build to `main.cpp` + `protocol.cpp`; the untracked bridge
  scaffolding (`settings.*`, `statusled.*`, `light.h`, `web/`) must NOT compile
  into the POC.
- **Serial port:** `/dev/ttyACM0` or `/dev/ttyACM1` — **it can swap on
  power-cycles** (`ls /dev/ttyACM*`; override with `ESP_SERIAL_PORT=...`).
  `monitor_port` in platformio.ini is stale.
- **DTR/RTS from the host does NOT reset this board** (probed 2026-11-16).
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

## 7. Standing rules / gotchas

- **DO NOT** loop/retry network calls or pin toggles — stop and report on denial.
- The `#if ENABLE_WIFI` blocks (HA/MQTT/HTTP) are **dead code in the POC** but must
  keep compiling when flipped to 1 — the HA lib (`dawidchyrzynski/home-assistant-
  integration`) is commented out of [platformio.ini](platformio.ini) and re-added then.
- `protocol.md` is the wire-format source of truth — don't copy frame layout back
  here.
