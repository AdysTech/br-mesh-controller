# CLAUDE.md — BRMesh → Home Assistant controller (ESP32-S3)

ESP32-S3 firmware that bridges **BRGD/BRMesh BLE lights** into **Home Assistant**
over MQTT. **Phases 0–1 done** (single build, one env): BLE FastCon engine +
serial console + WiFi STA with **Improv BLE** provisioning + **web shell**
(HTTP `/api/*` + WS `/ws/log`, SPA-ready LittleFS). No SPA yet (Phase 2), no
MQTT/HA yet (Phase 3).

**Read the siblings before touching code — they are the sources of truth:**
- [protocol.md](protocol.md) — the FastCon air protocol, byte-level (framing,
  command types, control payloads, factory-identity frame), how it was captured,
  and the two community references it builds on.
- [BRIDGE_PLAN.md](BRIDGE_PLAN.md) — the forward build (web UI, MQTT/HA, phases).

This file keeps only the model, current status, the catch recipe, console, and
toolchain.

**Code layout — a 3-layer split (keep `main.cpp` lean for the bridge stage):**
- [src/MeshController.{h,cpp}](src/MeshController.h) — the engine: key + lights + BLE advertise/scan + discovery/keying + NVS persistence
- [src/console.{h,cpp}](src/console.h) — the serial console driver; consumes a `MeshController&`
- [src/main.cpp](src/main.cpp) — thin app shell: setup/loop + §5 boot gate (NVS creds → STA → Improv)
- [src/settings.{h,cpp}](src/settings.h) — NVS-backed settings (creds, name, MQTT) · [src/statusled.{h,cpp}](src/statusled.h) — WS2812 status LED (native `rgbLedWrite`)
- [src/protocol.{h,cpp}](src/protocol.h) — FastCon byte codec (unchanged) · [src/light.h](src/light.h) — shared `LightDevice`
- [src/FirmwareVersion.h](src/FirmwareVersion.h) — single source of truth for the version (bump per phase — lora-relay convention)
- [src/web/](src/web/) — web shell: `WebLog` ring buffer · `ApiHttp` helpers · `WebServer` (httpd, routes, SPA handler, `/ws/log`)

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

## 4. Serial console (115200 baud)

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
prov                  clear saved WiFi creds + reboot into Improv provisioning
hostname [name]       show / set device name (DHCP hostname; NVS 'name')
showcfg | set <k> <v> config: name/ssid/pass/mhost/mport/muser/mpass (NVS)
```

**Provisioning boot:** on a boot where WiFi is unprovisioned/failing, the mesh
engine is NOT started — Improv owns the BLE radio for that boot, so the radio
commands above are rejected ("engine offline (provisioning mode)"). `prov`,
`list`, `help` etc. still work.

## 5. Build / flash / serial toolchain

- **Only working pio/python:** `/home/rao/.platformio/penv/bin/pio` and
  `/home/rao/.platformio/penv/bin/python`.
- Board `waveshare_esp32_s3_zero` (pioarduino core, 4 MB flash / 2 MB PSRAM),
  native USB: `-DARDUINO_USB_MODE=1 -DARDUINO_USB_CDC_ON_BOOT=1`.
- **One board, one build** (`waveshare_esp32_s3_zero`). `build_src_filter` in
  [platformio.ini](platformio.ini) scopes it to `main.cpp` + `protocol.cpp` +
  `MeshController.cpp` + `console.cpp` + `settings.cpp` + `statusled.cpp` +
  `web/*` (+ the shared `light.h`). There is no separate POC / `ENABLE_WIFI=0`
  build — the flag is gone.
- **Partitions (lora-relay shape, 4 MB):** dual OTA slots (1.81 MB each) + A/B
  spiffs SPA slots (96 KB; `spa_a` active — `board_build.filesystem_partition`).
  FS partitions must be **`data, spiffs`** — this core's LittleFS does NOT
  mount a `data, fat` partition (verified; lora-relay uses spiffs).
  `pio run -t uploadfs` flashes the SPA slot (Phase 2).
- **Verify from this host:** the board is `192.168.40.51` on VLAN 40 — mDNS
  (`.local`) does NOT cross VLANs here. Use the bare hostname:
  `curl http://brmesh/api/status` (resolves via the `nammane.home.arpa`
  search domain) or the IP directly.
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
- **No `ENABLE_WIFI` flag** — it was removed when the two builds collapsed into one
  (one board, one firmware). The HA lib (`dawidchyrzynski/home-assistant-integration`)
  is commented out of [platformio.ini](platformio.ini) and re-added in **Phase 3**.
  Do NOT re-introduce the build-flag split.
- **Firmware version:** single source of truth is
  [src/FirmwareVersion.h](src/FirmwareVersion.h) (`FIRMWARE_VERSION` +
  `kBoardModel`, current release only — no changelog). Bump it there when a
  phase ships; the boot banner, Improv device info, and `/api/status`
  (`fw`/`board`) all read it. Phase 2's `build_spa.py` bakes it into the SPA
  (lora-relay regex) for the mismatch check.
- `protocol.md` is the wire-format source of truth — don't copy frame layout back
  here.
- **Core hostname quirk (verified on hardware):** `WiFi.setHostname()` only
  writes a static buffer; the core applies it to the STA netif **inside**
  `WiFi.mode(WIFI_STA)` (the first enabling transition of a boot — after that a
  later `setHostname` never reaches the netif; symptom: DHCP advertises the
  generated `esp32s3-<mac>`). So the boot path must order
  `setHostname(name)` **before** `WiFi.mode(WIFI_STA)`, and a runtime change
  (console `hostname`) must go through
  `Network.getDefaultInterface()->setHostname()`.
- **S3-Zero WS2812B is physically GRB** despite the variant claiming
  `RGB_BUILTIN_LED_COLOR_ORDER=RGB` (verified: intended red showed as teal).
  Use `rgbLedWriteOrdered(RGB_BUILTIN, LED_COLOR_ORDER_GRB, r, g, b)` — see
  [src/statusled.cpp](src/statusled.cpp).
