# CLAUDE.md — BRMesh → Home Assistant controller (ESP32-S3)

ESP32-S3 firmware that bridges **BRGD/BRMesh BLE lights** into **Home Assistant**
over MQTT. **Phases 0–3 implemented** (single build, one env): BLE FastCon engine +
serial console + WiFi STA with **Improv BLE** provisioning + **web shell**
(HTTP `/api/*` + WS `/ws/log`) + **Svelte SPA** (Lights/Config/Console/Info) +
**HA bridge** (standard device-based MQTT discovery, `src/HaBridge.*`; live-broker
verification pending).

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
- [src/HaBridge.{h,cpp}](src/HaBridge.h) — Phase 3 HA bridge: one esp-mqtt client, all device-based discovery/state publishing behind it (hub + per-bulb), the `mname` identity (`dev.cu` always derived = `http://<board IP>/`)
- [src/main.cpp](src/main.cpp) — thin app shell: setup/loop + §5 boot gate (NVS creds → STA → Improv)
- [src/settings.{h,cpp}](src/settings.h) — NVS-backed settings (creds, name, MQTT) · [src/statusled.{h,cpp}](src/statusled.h) — WS2812 status LED (native `rgbLedWrite`)
- [src/protocol.{h,cpp}](src/protocol.h) — FastCon byte codec (unchanged) · [src/light.h](src/light.h) — shared `LightDevice`
- [src/FirmwareVersion.h](src/FirmwareVersion.h) — single source of truth for the version (bump per phase — lora-relay convention)
- [src/web/](src/web/) — web shell: `WebLog` ring buffer · `ApiHttp` helpers · `WebServer` (httpd, routes, SPA handler, `/ws/log`) · `ApiLights` (scan + light ops) · `ApiSystem` (config/provision/reset/raw)
- [frontend/](frontend/) — Svelte 5 SPA (Phase 2): `src/pages/` (Lights · Config · Console · Info), `src/lib/` (api · ws · router), `e2e/` (puppeteer smoke)

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
- **Bulb reset = the boot-time pairing window (no physical reset).** A bulb *persists*
  its key across reboots but re-enters a pairing window on **every** boot: WAKE heard
  in that window → it accepts a new key (SET_KEY); no WAKE → it keeps its stored key.
  So (re)keying a bulb = **reboot it + WAKE on the air** (`scan`) — and there is **no
  no-air "re-add"** (reboot + `scan` recovers a lost list entry). protocol.md §2.9.
- **Mesh relaying (the forward bit).** Bulbs don't ACK but they **re-broadcast** a
  command they hear — that relay is what carries one controller broadcast across the
  mesh. Header `p[0]` bit 7 (0x80) is the **forward/relay flag**: CONTROL goes out
  fwd=1, WAKE/SET_KEY fwd=0. protocol.md §2.8.

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
  (on the Tasmota power switch, can be controlled by claude). `2a:e6` / `0b:ed` are
  addable/controllable too — just not used as test targets.

- **Next:** the bridge per [BRIDGE_PLAN.md](BRIDGE_PLAN.md).

## 3. Discovery / keying (the proven catch recipe)

The factory-identity frame is only emitted in the seconds after power-up, and
only while WAKE is on the air. Proven recipe:

1. `scan 12` (duration configurable)
2. wait ~1 s → Tasmota `Power OFF`
3. wait ~2 s → `Power ON` — bulb boots *into* the ongoing WAKE with the full
   window left to advertise

**NimBLE can't advertise and
scan at once** (the ESP32 controller drops the advertiser when the scan starts),
so `scan` uses [slicedAdvertiseScan()](src/MeshController.cpp) — a WAKE/SET_KEY advertise
burst → scan burst loop — instead of the old concurrent Bluedroid pattern. `wantDuplicates=true`
still matters: the bulb interleaves bare beacons with identity frames and dedup
drops them. Keying (`addLight`) time-slices the same way. If the NVS light list is
ever lost: **reboot + `scan`** re-keys/re-confirms it (protocol.md §2.9).

## 4. Serial console (115200 baud)

`help` for the full list. Essentials:

```
list                  known lights            key                   current mesh key
sel <n>               target light            on/off/br/ct/rgb ... control (see protocol.md §2.5)
scan [sec=5]          discovery + key new bulbs (use 12+ with a mid-scan power-cycle)
raw [sec]             unfiltered per-frame hex dump (debug)
rawwake [sec]         WAKE broadcast + every frame (proved WAKE triggers identity)
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
- [tools/staged_capture.py](tools/staged_capture.py) — ENTER-driven staged passive capture
  (hunt driver for the phone's one-shot group frames; `scanstop` ends a stage).
  Capture logs land in `tools/capture_*.log`.
- [tools/decode_fastcon.py](tools/decode_fastcon.py) — **the FastCon decoder**: inverts
  protocol.cpp's encode pipeline (preamble restore, LFSR de-whiten seed `0x25`), reads
  the command TYPE off a captured payload without needing the command key. Modes:
  `decode_fastcon.py <hex...>` (one payload), `--key AABBCCDD` (explicit mesh key),
  `--file cap.log` (auto-recover key + distinct payloads), and `--relay cap.log`
  (per-frame table + fwd/seq summaries + the **relay-pair hunt** — same type/i2/seq
  seen with a flipped forward bit or 2 checksums = evidence of bulb re-broadcast).

## 6b. SPA build + E2E (Phase 2)

- **Build:** `cd frontend && npm run build` → `vite` inlines JS+CSS into one
  `html/index.html`; `tools/build_spa.py` gzips it to `data/index.html.gz` +
  writes `data/spa.version`. `pio run --target upload` / `uploadfs` flash
  firmware + SPA. Board serves `data/index.html.gz` with `Content-Encoding: gzip`.
- **E2E:** `frontend/e2e/smoke.mjs` — puppeteer against the cached
  `chrome-headless-shell` (lora-relay's harness). Loads the SPA, asserts no
  page/console errors + all 4 pages render + one light-toggle round-trip, then
  (with a registered bulb) exercises the P2.10 controls **end-to-end against
  the board**: colour-wheel drag → `believed_state.rgb`, white-LED CT slider →
  `believed_state.mode=ct`, and a refresh restoring believed state (no white
  box) — each a single board-bound action, asserting the board's OWN
  `/api/lights` `believed_state` moved (UI → POST → controller → GET).
  `E2E_APP_URL=… node e2e/smoke.mjs` (default `http://brmesh`).
  **Run this after any SPA change** — it is the blank-page regression guard
  AND the NFR-07 ship gate.

## 7. Standing rules / gotchas

- **Trust this file as the repo map.** If a tool isn't listed in §6 it doesn't exist —
  do NOT re-scan the repo for prior art or re-derive the codec/whitening that
  `protocol.md` + [tools/decode_fastcon.py](tools/decode_fastcon.py) already define.
  Start from the map, not from a search. Keep §6 current when you add tooling.
- **DO NOT** loop/retry network calls or pin toggles — stop and report on denial.
- **No `ENABLE_WIFI` flag** — it was removed when the two builds collapsed into one
  (one board, one firmware). **Phase 3 HA bridge: standard MQTT device-based
  discovery** (plain retained JSON, HA ≥ 2026.4 key set — `uniq_id`, `via_device`,
  `dev.cu`, no `object_id`) over esp-mqtt (ESP-IDF core `esp_mqtt_client`, no
  third-party lib), per [BRIDGE_PLAN.md](BRIDGE_PLAN.md)
  §7.2 — the `home-assistant-integration` lib is NOT used (classic per-component
  discovery). Do NOT re-introduce the build-flag split.
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
- **Svelte 5: never name a variable `state` (or `$state`'s target) in a runes file.**
  With a `state` binding in scope, `$state(...)` compiles as a legacy `$` store
  access instead of the rune → runtime `store_invalid_shape`, blank page
  (caught 2026-11-19: `function state(l)` in LightsPage). Name helpers `st`, `opt`,
  etc. — not `state`/`$state`-adjacent names.
- **`httpd_req_get_url_query_str` returns `esp_err_t` (`ESP_OK == 0`).**
  The success check is `!= ESP_OK`, NOT `!ret` (the old inverted check made
  `?n=` silently empty — every light op 400'd). See [src/web/ApiHttp.cpp](src/web/ApiHttp.cpp).
