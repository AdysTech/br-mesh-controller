# BRMesh → HA Bridge — System Design & Build Plan

> Durable plan, restyled 2026-11-18 after `../lora-relay/SPA-Web-Gate-Controller-Design.md`
> so development is linear and focused: design sections first (locked decisions),
> then phases in build order, each with a task table and a concrete deliverable.
>
> **Read [CLAUDE.md](CLAUDE.md) first** for the proven POC, the catch recipe, and
> the serial toolchain. [protocol.md](protocol.md) is the wire-format source of truth.
> This file is the **forward build** (the bridge) that the POC enables.

## Table of Contents

1. [Requirements](#1-requirements)
2. [System Topology](#2-system-topology)
3. [Software & Tech Stack (locked)](#3-software--tech-stack-locked)
4. [Storage & Data Layout](#4-storage--data-layout)
5. [Boot Flow & State Machine](#5-boot-flow--state-machine)
6. [Web API Surface](#6-web-api-surface)
7. [HA Integration Design](#7-ha-integration-design)
8. [Command Queue & Group Transactions](#8-command-queue--group-transactions)
9. [Discovery, Keying & Runtime Constraints](#9-discovery-keying--runtime-constraints)
10. [Raw Capture Path](#10-raw-capture-path)
11. [Risks & Mitigations](#11-risks--mitigations)
12. [Implementation Phases](#12-implementation-phases)
12. [Appendix A — Proven Baseline (do not re-litigate)](#appendix-a--proven-baseline)
13. [Appendix B — Toolchain](#appendix-b--toolchain)
14. [Appendix C — Finding F-CT: WW/CW colour-temperature model](#appendix-c--finding-f-ct--wwcw-colour-temperature-model)

---

## 1. Requirements

### 1.1 Functional Requirements

| ID | Requirement | Priority |
|----|-------------|----------|
| FR-01 | The controller shall self-provision its WiFi credentials over BLE (Improv) on first boot, then stay on the LAN | Mandatory |
| FR-02 | Each keyed bulb shall be exposed to Home Assistant as an individual light entity over MQTT (auto-discovery): on/off, brightness (0–127), RGB | Mandatory |
| FR-03 | The controller shall expose an HA button "Start scan" and status sensors (scan state, lights count, WiFi RSSI, MQTT link) | Mandatory |
| FR-04 | A Tasmota-style web UI shall be served over HTTP: status, lights, scan, config, console — reachable at `<name>.local` and the LAN IP | Mandatory |
| FR-05 | State shall be **believed state** — the ESP tracks the last command sent per light (broadcast-only bulbs, no readback) | Mandatory |
| FR-06 | A raw FastCon packet-capture path shall exist on serial **and** in the web console, triggerable from the UI (field path for new bulb models) | Mandatory |
| FR-07 | Key, light list, and all config (name, WiFi, MQTT) shall persist in NVS across reboots; no re-keying on power-cycle | Mandatory |
| FR-08 | The serial console (POC command set: `list`, `sel`, `on/off/ct/rgb`, `scan`, `raw`…) shall keep working in bridge mode | Mandatory |
| FR-09 | Colour + white control (bulbs advertise type `2A A8`) — **the on-hand bulbs are RGBW**: one op `0x72`, one 8-byte payload, two mutually-exclusive modes — **RGB** (`d[3]=B,d[4]=R,d[5]=G`, white off) **or** **WW/CW white crossfade** (`d[6]=CW, d[7]=255−CW`, RGB off), not a single-axis CT ([protocol.md](protocol.md) §2.5b): the UI ships a colour/white control in Phase 2 (P2.10), and the firmware now encodes the correct unified `0x72` wire (RGB or CW/WW crossfade, `d[2]=0xff`) — **Finding F-CT fixed 2026-11-20**, kelvin unit adopted 2026-11-22 (Appendix C): `ct` is a kelvin label (2700–6000 K) end-to-end, split to the WW/CW crossfade only at TX. Remaining: the fine kelvin→byte mid-curve (LUT) is Phase 5 (P5.2) | Phase 2 UI (P2.10) + correct wire + kelvin unit (done); fine mid-curve LUT Phase 5 (P5.2) |
| FR-10 | "All lights" **virtual selection** (ESP-owned fan-out entity, every keyed bulb) — §7.3 L2 | Later (Phase 5) |
| FR-15 | **Native room groups (§7.3 L1):** a bulb is assigned to a physical room at keying; whole-room command = one FastCon group frame. **Verify-first** on the `0x21` assign + type-3 group frames (not yet in protocol.md); until verified, a room group is a virtual selection (L2) | Later (Phase 5, gated on verification) |
| FR-16 | **State convergence:** a bulb is broadcast-only and may have missed a command (unpowered when we sent it), so the controller shall re-transmit each light's last known state on a fixed cadence (default ~10 s, tunable) so the air converges to what the user set; off must stay off (never re-send a colour frame to an off bulb); skipped while scanning (radio is time-sliced) | Phase 2 (P2.9) |
| FR-17 | **Observable logging:** a leveled logger (error/info/debug) with a settable level and per-line uptime timestamps so the operator can read what happened and how long it took; quiet by default, detailed on demand — modeled on lora-relay's `Shared.h` logger | Phase 2 (P2.11) |
| FR-11 | Firmware / LittleFS OTA via the web UI | Optional |
| FR-12 | A light entry shall be deletable from the web UI (and console); its HA entity shall be removed from Home Assistant | Mandatory |
| FR-13 | A light shall be **renamable** from the web UI (and console), modeled on zigbee2mqtt `friendly_name`: the HA entity's name updates in place (no orphan entity), and the rename persists | Mandatory |
| FR-14 | Group-level transactions (HA group toggles, fan-out, UI bursts) shall be buffered in a bounded command queue and drained as paced, sequenced broadcasts — the radio never receives unsynchronized bursts; queue depth is observable | Mandatory |

### 1.2 Non-Functional Requirements

| ID | Requirement | Target |
|----|-------------|--------|
| NFR-01 | Total gzipped SPA payload (HTML+CSS+JS) | Under 100 KB (896 KB LittleFS slot, keep headroom) |
| NFR-02 | HTTP server shall not block BLE operations or MQTT | `esp_http_server` on its own FreeRTOS task |
| NFR-03 | One board, one build: the single env (`waveshare_esp32_s3_zero`) shall keep compiling; the serial console remains the fallback front-end throughout | Build config |
| NFR-04 | No key extraction from the Android app; the ESP is the key originator | Project rule |
| NFR-05 | No network/pin retry loops — one attempt, stop and report on denial | Project rule |
| NFR-06 | `protocol.cpp` stays stack-agnostic and byte-identical to the proven reference | Untouched |
| NFR-07 | **The SPA is ship-gated by UI-based (browser) tests** — `frontend/e2e/smoke.mjs` (puppeteer against the cached headless shell): no page/console errors, all four pages render, one light on/off round-trip, and each new control exercised end-to-end — colour wheel and white-LED (CT) slider drive the board's own `believed_state` (UI → POST → controller → GET) and a refresh restores it (P2.8). Any SPA change must pass the e2e before it ships; a new control ships with its e2e case | Test gate |

### 1.3 Hardware Assumptions

| Component | Board | Notes |
|-----------|-------|-------|
| Controller | Waveshare ESP32-S3-Zero | 4 MB flash (2 MB app + 896 KB LittleFS), 2 MB PSRAM, native USB serial, WS2812B status LED on **GPIO 21** |
| Lights | N × BRMesh BLE bulb, OUI `f2:72:70:e8`, type `2A A8` | Design targets any N. On hand: 1 keyed (`70:e7…`, on Tasmota power) plus a stock of factory-new bulbs for the add/remove lifecycle proofs |

---

## 2. System Topology

```
┌─────────────────────────────────────────────────────────┐
 │              Home Assistant (MQTT broker)               │
 │  • One light entity per keyed bulb (auto-discovery)    │
 │  • Controller button "Start scan" + status sensors     │
 └───────────────▲───────────────────────────▲────────────┘
                 │ MQTT 1883                 │
┌────────────────┴───────────────────────────┴────────────┐
 │              Waveshare ESP32-S3-Zero                    │
 │  • WiFi STA (Improv-provisioned creds, NVS)            │
 │  • esp_http_server (REST + /ws/log, LittleFS SPA)      │
 │  • MQTT bridge: hub device + one device per bulb       │
 │    (standard device-based discovery, §7.2)             │
 │  • MeshController: FastCon engine (NimBLE adv/scan)    │
 │  • Serial console (115200) — POC command set, always   │
 └───────────────▲────────────────────────────────────────┘
                 │ BLE advertising (broadcast, no pairing)
                 ▼
┌─────────────────────────────────────────────────────────┐
 │        BRMesh bulbs (N × factory OUI f2:72:70:e8)       │
 │  • Listen to FastCon broadcast frames only             │
 │  • Emit factory-identity frame on power-up (keying)    │
 └─────────────────────────────────────────────────────────┘

 Browser (phone/PC) ──HTTP/WS──▶ esp_http_server  (Tasmota-style UI)
```

---

## 3. Software & Tech Stack (locked)

| Aspect | Choice | Rationale |
|--------|--------|-----------|
| BLE host | **NimBLE-Arduino 2.x** (`h2zero/NimBLE-Arduino@^2.5.1`) | Migrated 2026-11-16; proven re-re-proven for detect→key→control (2026-11-18). Improv provisioning is also NimBLE → **one host stack, no link conflict**. |
| Bulb protocol | [protocol.cpp](src/protocol.cpp) — FastCon byte codec | Byte-identical to the proven reference (protocol.md §3). Untouched. |
| Engine | [MeshController.\*](src/MeshController.cpp) — key + lights + BLE advertise/scan + NVS persistence | Time-sliced WAKE/SET_KEY advertise → scan burst (NimBLE drops the advertiser when a scan starts). |
| Serial console | [console.\*](src/console.cpp) | POC command set; stays in every build. |
| HTTP server | **`esp_http_server`** (ESP-IDF native), mirroring `../lora-relay` | User mandate: not the Arduino `WebServer`. `httpd_uri_t` routes, chunked/gzip, native WebSocket. |
| Frontend | **Svelte + Vite**, `vite-plugin-singlefile` → one gzipped `index.html` → LittleFS | User mandate: reuse lora-relay's pipeline. `tools/build_spa.py` pre-action. |
| Filesystem | LittleFS (`board_build.filesystem = littlefs`, 896 KB slot in [partitions.csv](partitions.csv)) | Already configured. |
| Config | **NVS** (`Preferences`, namespace `brmesh`) — name, WiFi, MQTT | No hardcoding. [settings.\*](src/settings.cpp) landed. |
| HA bridge | **Standard HA MQTT *device-based* discovery** (plain retained JSON, HA ≥ 2026.4 key set) over **`esp-mqtt`** (ESP-IDF core `esp_mqtt_client`) + one discovery/state JSON builder module — no third-party MQTT or HA library | `esp-mqtt` gives the async FreeRTOS task, QoS 0/1/2 with retransmit outbox, built-in exponential-backoff reconnect + LWT + session resume, and a configurable buffer (`buffer.size` / `outbox`) for the discovery payloads; MQTT 3.1.1 or 5.0. HA subscribes to the standard `homeassistant/device/<uid>/config` topics (§7.2): one controller-hub device + one device per bulb, linked by `via_device`, `dev.cu` → the Svelte SPA. `dawidchyrzynski/home-assistant-integration` is **not used** (it emits classic per-component discovery) — stays commented out in [platformio.ini](platformio.ini). |
| Provisioning | **Improv BLE** (`jnthas/Improv-WiFi-Library`, NimBLE) — lora-relay's proven route | Runtime-gated: skipped when STA already connects. Fallback if it fails: captive portal (SoftAP + DNSServer → `/setup`), keeps the bulb stack untouched. |
| Groups | **Two layers (§7.3):** native room groups (ESP NVS + bulb flash, 1-frame) **+** virtual selections (ESP- or HA-owned fan-out) | Native 1-frame path is verify-first (§7.3); virtual fan-out via the §8 queue. Phase 5. |
| Status LED | WS2812B, GPIO 21, `Adafruit_NeoPixel` 1 px ([statusled.\*](src/statusled.cpp)) | boot=white, scanning=blue, keying=amber, wifi=green, prov=red, error=fast-red. |
| JSON/WS plumbing | [web/ApiHttp.\*](src/web/ApiHttp.cpp), [web/WebLog.\*](src/web/WebLog.cpp) (ring buffer + WS broadcast) | Mirrors lora-relay `ApiHttp`; landed. |

---

## 4. Storage & Data Layout

### 4.1 Flash Partitions (4 MB board)

lora-relay proven shape (dual OTA + A/B SPA slots; see [partitions.csv](partitions.csv)):

| Partition | Size | Purpose |
|-----------|------|---------|
| nvs | 20 KB | WiFi creds, MQTT host/port/user/pass, controller name, mesh key, light list |
| otadata | 8 KB | Active OTA slot metadata |
| phy_init | 4 KB | RF/PHY calibration data |
| ota_0 | 1.81 MB | Firmware slot A |
| ota_1 | 1.81 MB | Firmware slot B |
| spa_b | 96 KB | Inactive SPA slot (`index.html.gz` A/B) |
| spa_a | 96 KB | Active SPA slot — listed LAST so `pio run -t uploadfs` targets it (`board_build.filesystem_partition = spa_a`) |

### 4.2 Data Stores

| Store | Contents | Access Pattern |
|-------|----------|----------------|
| **NVS** `brmesh` | name; wifi ssid/pass; mqtt host/port/user/pass; mqtt **hub name `mname`** (HA `dev.name`; empty → device `name`); 4-byte mesh key; light list (id, **name**, mac, type, features) | Fast read/write; **write paths must open with `readOnly=false`** (proven gotcha, CLAUDE.md §2). `dev.cu` is NOT stored — always derived at publish time as `http://<board IP>/` |
| **LittleFS** | `index.html.gz` — single-file SPA | Served over HTTP; replaceable via UI upload |
| **SRAM** | Believed state per light (last sent on/brightness/rgb), scan state, WS client registry, log ring | Volatile; believed state is the source of HA state |

---

## 5. Boot Flow & State Machine

```
 Boot
   │
   ▼
 NVS: load name + WiFi creds + MQTT config
   │
   ▼
 statusLED: white (boot)
   │
   ▼
 WiFi STA with saved creds (bounded timeout)
   │
   ├── CONNECTED ────────────────────────────────────────┐
   │                                                      ▼
   │                     mDNS <name>.local
   │                                                      ▼
   │                     esp_http_server up (SPA, REST, WS)
   │                                                      ▼
   │                     MQTT host in NVS (mhost)? ── no ──▶ skip HA (web+serial only)
   │                        │ yes
   │                        ▼
   │                     MQTT connect → birth + LWT set, hub + bulb
   │                     discovery published (AFTER the connect — order is
   │                     load-bearing; re-published on homeassistant/status → online)
   │                                                      ▼
   │                     loadLights() → restore believed state → IDLE (green)
   │
   └── FAILED (no creds / no network, bounded time)
            ▼
     statusLED: red — Improv BLE provisioning on
            ▼
     phone (Improv scanner/app) hands over ssid+pass
            ▼
     creds → NVS, reboot → back at "WiFi STA" (now connected)

 Re-entry: console `prov` / web "Provision via BLE" → clear saved WiFi → reboot
 Factory reset: console `factoryreset` / web → wipe NVS brmesh + reboot
```

| State | LED | Meaning |
|-------|-----|---------|
| BOOT | white | NVS/network bring-up |
| IDLE | green | WiFi up, bridge running |
| SCANNING | blue (pulse) | discovery scan in progress |
| KEYING | amber | SET_KEY / identity exchange |
| PROV | red (solid) | Improv active |
| ERROR | red (fast) | crash-path / MQTT down |

**Constraints:** scan and advertising cannot run concurrently on this NimBLE controller —
all BLE activity goes through the time-sliced advertise-burst → scan-burst loop
([slicedAdvertiseScan()](src/main.cpp) today, moving to `MeshController`).

---

## 6. Web API Surface

All JSON via [web/ApiHttp.\*](src/web/ApiHttp.cpp) helpers (`apiSendJson`, `apiSendError`, `apiReadBody`).

| Method | Path | Purpose |
|--------|------|---------|
| GET | `/` (+ `/*` catch-all) | gzipped `index.html` from LittleFS, `Content-Encoding: gzip`, no-cache |
| GET | `/api/status` | name, mode (sta/prov), ip, rssi, ssid, mqtt up, lights count, scan state, fw, uptime, heap |
| GET | `/api/lights` | `[{n, name, id, mac, type, features[], believed_state}]` |
| POST | `/api/scan` | body `{seconds?}` → start discovery + key new bulbs (guarded: rejected while scanning) |
| POST | `/api/light/{n}` | body `{on}` / `{brightness:0-127}` / `{ct}` / `{rgb:[r,g,b]}` |
| DELETE | `/api/light/{n}` | remove the light entry (NVS) + remove its HA entity (FR-12) |
| PATCH | `/api/light/{n}` | body `{name}` — rename the light + re-publish its HA discovery so HA renames the entity in place (FR-13) |
| GET | `/api/config` | read NVS settings (name, wifi, mqtt) — passwords masked |
| POST | `/api/config` | save NVS settings; triggers reboot where required |
| POST | `/api/provision` | clear saved WiFi → reboot into Improv |
| POST | `/api/factory-reset` | wipe NVS `brmesh` + reboot (re-keys everything) |
| POST | `/api/raw` | body `{sec?, wake?}` → raw FastCon capture into the log ring (see §10) |
| WS | `/ws/log` | live log stream (ring buffer + tail) → the console page |

**WS message schema** (single channel, typed):

```json
{ "type": "log",        "level": 1, "text": "scan: 12 frames, 2 identity" }
{ "type": "scan_state", "state": "scanning", "frames": 42, "t_left_s": 7 }
{ "type": "light",      "n": 1, "on": true, "brightness": 100 }
{ "type": "status",     "rssi": -52, "mqtt": true }
```

Client reconnects with exponential backoff (1/2/4/8 s, cap 30 s); the log ring
serves the last N lines on connect so a reconnect loses nothing.

---

## 7. HA Integration Design

### 7.1 Entity Model

| Entity | HA component | Lives on which device |
|--------|-------------|-----------------------|
| One light per keyed bulb (user-renamable, FR-13) — on/off, brightness, RGB first; CT in Phase 5 (P5.2) | `light` | **the bulb's own device** `homeassistant/device/<ctrl_id>_l<n>/config`, linked back with `via_device` (§7.2) |
| `Start scan` | `button` | controller-hub device |
| `Scan state` (IDLE/SCANNING/KEYING), `Lights registered`, `WiFi RSSI`, `MQTT link`, `Queue depth` (diagnostic) | `sensor` | controller-hub device |
| **Room group** (physical, e.g. `living_room`) — §7.3 L1 | `light` (ESP-owned) | controller-hub device; native group frame = **1 TX** once `0x21`/type-3 verified; else L2 batched fan-out (Phase 5) |
| **Virtual selection** (e.g. `All lights`, FR-10) — §7.3 L2 | `light` (ESP-owned) | controller-hub device; fan-out through the §8 queue (Phase 5) |
| **Virtual selection** (e.g. `TV rim`) — §7.3 L2 | HA `group`/`scene` (HA-owned) | HA-side composition over the light entities; HA fans out → §8 queue |

> **Granularity rule (multi-controller scale):** the controller registers
> **itself as one hub device** (diagnostics, `Start scan`, ESP-owned selections)
> and **each bulb as its own device**, linked back via `"via_device": "<ctrl_id>"`
> — HA's device registry shows controller → bulbs. Multiple controllers coexist
> on the same broker; `<ctrl_id>` keeps them apart. Never lump several bulbs into
> one discovery payload. The hub's `dev.cu` carries the Svelte SPA URL
> (default `http://<board IP>/`, overridable in the Config page) so HA links
> straight to the UI without knowing the IP.

> **A group is a separate entity, not a field on the light.** The individual light
> entity carries **no `group_id`** — a group is either its own ESP entity (room /
> `All lights`) or an HA-side composition (scene). The native `group_id` is
> ESP/bulb-internal state that powers the 1-frame room path (§7.3); it is never a
> payload HA relies on.

### 7.2 Topic Layout (standard device-based discovery)

**Identity (stable — never derived from names):**
- `<ctrl_id>` = `brmesh_<last 6 hex of MAC>` (e.g. `brmesh_1a2b3c`) — unique even
  with several controllers on one broker, unchanged by rename or factory reset.
- Bulb device id = `<ctrl_id>_l<n>` (n = the light's number — stable; never the name).
- Runtime topic prefix = `brmesh/<ctrl_id>/…`.

**Discovery namespace** — HA *device-based* discovery
(`homeassistant/device/<uid>/config`), retained, QoS 1. HA ≥ 2026.4 key set:
abbreviated keys (`dev` / `o` / `avty` / `cmps`), `uniq_id` on every component,
**no `object_id`** (removed in HA 2026.4).

| What | Topic |
|------|-------|
| Controller-hub device | `homeassistant/device/<ctrl_id>/config` — `dev` (`ids`, `name`, `mf`, `mdl`, `sw`, **`cu`** = SPA URL), `o` origin, `avty`, `cmps`: `Start scan` `button`, `scan state` / `lights registered` / `WiFi RSSI` / `MQTT link` / `queue depth` (diagnostic) sensors, (Phase 5: ESP-owned group/selection `light`s) |
| Bulb device — one per keyed bulb | `homeassistant/device/<ctrl_id>_l<n>/config` — `dev` with `via_device: <ctrl_id>`, `cmps.main` = `light` (on/off, brightness, RGB from the state JSON; CT arrives P5.2) |
| Removal (FR-12) | publish `""` (empty), retained, to the bulb's config topic → HA purges the device + its entities |

**Runtime namespace:**

| Direction | Topic | Payload |
|-----------|-------|---------|
| HA → ESP | `brmesh/<ctrl_id>/l<n>/set` | command JSON — `{"on":true}` / `{"brightness":0-255}` (scaled to 0–127 at TX) / `{"rgb":[r,g,b]}` / `{"ct":<kelvin>}`; subscribe `brmesh/<ctrl_id>/+/set` |
| HA → ESP | `brmesh/<ctrl_id>/scan` | `START` (the hub button's `cmd_t`) |
| ESP → HA | `brmesh/<ctrl_id>/status` | `"online"` / `"offline"` — birth + LWT, retained |
| ESP → HA | `brmesh/<ctrl_id>/l<n>/availability` | `"online"` / `"offline"`, retained |
| ESP → HA | `brmesh/<ctrl_id>/l<n>/state` | retained JSON `{"state":"ON","brightness":180,"color":[255,0,0],"ct":4000,"mode":"rgb\|white"}` — believed state (FR-05); the HA MQTT light reads `state`/`brightness`/`color` straight out of the JSON |
| ESP → HA | `brmesh/<ctrl_id>/state` | retained controller JSON `{scan_state, lights, rssi, mqtt, queue_depth, uptime}` — one topic, each hub sensor a `val_tpl` (`{{ value_json.rssi }}`, …) |

**Reference bulb payload** (the exact shape we publish):

```json
{
  "dev": {
    "ids": ["brmesh_1a2b3c_l1"],
    "name": "Ceiling",
    "mf": "BRGD",
    "mdl": "BRMesh bulb",
    "sw": "1.3.0",
    "via_device": "brmesh_1a2b3c"
  },
  "o": { "name": "brmesh-bridge", "sw": "1.3.0" },
  "avty": [
    { "t": "brmesh/brmesh_1a2b3c/status" },
    { "t": "brmesh/brmesh_1a2b3c/l1/availability" }
  ],
  "avty_mode": "all",
  "cmps": {
    "main": {
      "p": "light",
      "uniq_id": "brmesh_1a2b3c_l1_main",
      "stat_t": "brmesh/brmesh_1a2b3c/l1/state",
      "cmd_t": "brmesh/brmesh_1a2b3c/l1/set",
      "val_tpl": "{{ value_json.state }}",
      "bri_tpl": "{{ value_json.brightness }}",
      "clr_tpl": "{{ value_json.color }}"
    }
  }
}
```

**Rules:**
- **Follow HA's standard device-based schema** — abbreviated keys, `uniq_id`,
  `via_device`, `o`, `avty` arrays with `avty_mode: "all"`. No `object_id`, no
  per-component `homeassistant/light/<obj>/config` topics. It is plain retained
  JSON we publish; the schema is HA's, and we conform to it.
- **Stable identity (zigbee2mqtt model):** device `ids` + component `uniq_id` derive
  from `<ctrl_id>` + light number — *never* from the name. The name lives only in
  `dev.name`.
- **Rename (FR-13):** `rename <n> <text>` (console) / `PATCH /api/light/{n}` saves
  the name to NVS and re-publishes that bulb's config with the new `dev.name`. HA
  keeps the same device/entities (ids unchanged) and updates the name in place — no
  orphan, no re-pairing.
- **Controller rename is harmless:** `<ctrl_id>` is MAC-derived, so renaming the
  device only moves `dev.name` (one re-publish) — topics and entities don't rebind.
- **Lifecycle:** discovery publishes **after** the MQTT connect and again whenever a
  new bulb is keyed (dynamic discovery); the controller subscribes to
  `homeassistant/status` and a `"online"` payload triggers a **full re-publish** of
  discovery + retained state (HA restarted); a broker blip does the same on reconnect.
- **Availability:** the hub `status` topic (birth/LWT) + each bulb's `availability`
  feed the `avty` arrays (`avty_mode: "all"`), so a crash marks everything
  unavailable, not just one entity.
- **Believed state:** entity state = last command sent; no readback exists. Bulbs are
  broadcast-only — HA reflects commands optimistically, the ESP logs the send.
- **Removal (FR-12):** `del <n>` (console) / `DELETE /api/light/{n}` removes the NVS
  entry, saves, and publishes the empty-retained-payload purge above so the device
  disappears from HA. The physical bulb keeps its key — it still obeys our
  broadcast — it is simply no longer represented; re-addable any time
  (reboot the bulb + `scan`).

### 7.3 Group Model (two layers)

FastCon has no ACK and no readback, and — unlike HA — **a bulb can be in only one
native group**: the group id is a property the bulb stores in its flash via the
`0x21` assign frame (one field, last-write-wins → exclusive). HA groups, by
contrast, are **views** — a light can be in any number of them. These are
different abstractions, so the bridge splits groups into two layers and keeps
them in different places:

| Layer | What it is | Lives in | How it's commanded |
|-------|-----------|----------|--------------------|
| **L1 — Native group** | a **physical room/zone**; a bulb sits in exactly one | **ESP NVS + the bulb's flash** (assigned at keying) | **one FastCon group frame** (type 3) — 1 TX |
| **L2 — Virtual selection** | any overlapping/derived set (`TV rim`, `All lights`, `party`) | **ESP NVS** (ESP-owned) *or* **HA** (HA-owned) | **fan-out** to members, batched through the §8 queue |

**L1 — native group (the physical partition).** The room is the only group a
bulb *is*. "Dim the whole room" is a single group frame that every bulb in that
room acts on — no per-bulb fan-out, and the airtime cost is **flat regardless of
how many bulbs are in the room**. That is where the scaling win lives. Membership
is set **at keying** (send the assign frame to the new bulb) and is otherwise
stable — a bulb changes rooms only when it's physically moved.

> **Verify-first.** The assign (`0x21, light_id, group_id`) and group-control
> (type 3) frames come from an external reference — **not in
> [protocol.md](protocol.md) and not yet proven on these bulbs.** Until both are
> captured and verified, a "room group" is just an **L2 virtual selection**
> (batched fan-out) — correct, only slower. The 1-frame path is an optimization
> we switch on once verified.

**L2 — virtual selection (the views).** Any overlapping selection. Two ownership
options, both converge on the §8 queue:
- **ESP-owned** — a `light` component on the controller-hub device (e.g. the
  `All lights` fan-out, FR-10). Membership in NVS; the controller expands one
  command into N queue items, batched.
- **HA-owned** — a HA `group`/`scene` composed over the individual light entities
  in the HA UI (e.g. `TV rim`). HA fans out → N `set` messages → the §8 queue.
  Simplest for ad-hoc user selections; no controller state.

**Crisp decision rule:**
- **Physically co-located + exclusive + operated as a whole → L1 (native, 1 frame).**
- **Overlapping / derived / ad-hoc → L2 (virtual, fan-out)** — ESP-owned when it's a
  stable, large, frequently-toggled selection (batching worth it), HA-owned when it's
  an ad-hoc user selection.

**One seam:** the controller's control path is a single `sendToSet(set, op)`. If
`set` is a verified native group → emit the one group frame. Otherwise → expand to
members and enqueue (§8). HA never sees the difference; it just toggles an entity.

**Believed state (FR-05):** a group has no independent truth — its reported state
is **derived from its members' last-commanded state** (e.g. "on" only if all
members were last commanded on, else mixed). Toggling a member individually updates
the group's tracked state, so the two stay coherent. Still optimistic — no readback.

**Worked example (the ceiling):** `living_room` (all 8 ceiling bulbs) is **L1** —
one native group, whole-room dim = 1 frame. `TV rim` (the 4 near the TV) is **L2**
(HA-owned scene) — dim it and HA fans out to those 4. The 4 TV bulbs are in
**both**, and that's fine: *room* is a physical property (exclusive), *TV rim* is a
view (overlapping). No re-keying, no partition conflict.

---

## 8. Command Queue & Group Transactions

FastCon is **broadcast with no acknowledgement** (CLAUDE.md §1) — bulbs never reply,
so the queue is drained by **paced transmit slots**, not by acks. All command
sources (MQTT `set` topics, fan-out, web POSTs, serial) converge on one queue that is
the **sole owner of control airtime** (FR-14).

### 8.1 Problem

An HA group light toggled off by the user produces **N MQTT `set` messages within
milliseconds** (one per member). The radio is single and time-sliced; an
unsynchronized burst would collide with itself (and with a scan). So bursts must
be serialized and paced.

### 8.2 Design

| Aspect | Decision |
|--------|----------|
| Structure | Bounded FIFO (default **64 items**) in `MeshController`; each item = `{light n, op}` where op ∈ {on/off, brightness, rgb, ct} |
| Sources | MQTT `set` handlers, fan-out expansion (1 "All lights" msg → N items), `POST /api/light/{n}`, serial console — **all enqueue, none transmit directly** |
| Coalescing | Enqueue **replaces** any still-pending item for the same `(light, op-class)` → latest-wins; a spammy brightness slider leaves one item, not ten |
| Pacer | Drain loop fires **one item per slot**; slot gap `T_TX` (default **150 ms**, NVS-tunable); each item sent **R times** (default **2** — broadcast reliability without readback) |
| Accept semantics | Queue non-full ⇒ accepted immediately (MQTT: queued; web: `202 {queued: <pos>}`; serial: `queued`). Full ⇒ drop-oldest-per-light + log + `queue_overflow` in `/api/status` — never block the caller |
| Radio exclusivity | Drain and scan are mutually exclusive. Starting a scan requires the queue drained (bounded wait ≤ 1 s, else reject the scan). Enqueue during a scan is **rejected** with a clear error (matches the one-scan rule, §9) |
| "Done" | All slots sent — **there is no per-bulb ack to wait for** (protocol is broadcast-only). Believed state (FR-05) is stamped at *enqueue* so HA reflects a command instantly; airtime lag is bounded, not hidden |
| Observability | `queue_depth`, `queue_last_drain_ms`, `queue_overflow_count` in `/api/status`; WS push on drain start/end; optional HA sensor `Pending commands` (Phase 3) |

### 8.3 Timing budget (example)

| Group size | R | T_TX | Worst-case airtime | HA reflects |
|-----------|---|------|--------------------|------------|
| 5 lights | 2 | 150 ms | ~0.8 s | instantly (optimistic) |
| 10 lights | 2 | 150 ms | ~1.5 s | instantly (optimistic) |
| 20 lights | 2 | 150 ms | ~3 s | instantly (optimistic) |

A drain this short needs no dedicated LED state (IDLE green throughout); the
scan/keying states (§5) are unchanged.

### 8.4 Ordering guarantees

- FIFO across lights (bulb 1's item before bulb 2's) — deterministic, replayable.
- Per-light: latest-wins via coalescing — a bulb never receives an old brightness
  after a newer one.
- Group + individual mix: the queue is one line; an individual command queued
  mid-fan-out simply lands later, still latest-wins per light.

---

## 9. Discovery, Keying & Runtime Constraints

- The factory-identity frame is emitted only in the seconds after power-up, and
  only while WAKE is on the air — proven catch recipe in CLAUDE.md §3
  (`scan 12` + early power-cycle of the bulb).
- NimBLE drops the advertiser when a scan starts → everything time-slices:
  WAKE/SET_KEY advertise burst → scan burst loop.
- `wantDuplicates=true` matters: bulbs interleave bare beacons with identity frames.
- **List recovery:** a lost entry needs no special path — reboot + `scan` re-keys/re-confirms it (protocol.md §2.9); there is no no-air `readd`.
- **Runtime rule:** at most one scan at a time; scan start is guarded (console,
  web, HA button all hit the same single entry point in `MeshController`).

---

## 10. Raw Capture Path

`raw` / `rawwake` stay as serial commands. Their per-frame hex lines flow through
the `WebLog` ring so the same capture appears on:

- **serial console** (as today),
- **web `/console` page** via `/ws/log`,
- triggerable from the UI (`POST /api/raw {sec, wake}`) — the field path for
  reverse-engineering a new bulb model. Field drivers:
  [tools/raw_catch.py](tools/raw_catch.py), [tools/rawwake_catch.py](tools/rawwake_catch.py).

---

## 11. Risks & Mitigations

| Risk | Status | Mitigation |
|------|--------|------------|
| Improv vs bulb BLE stack conflict | **Largely resolved** — both are NimBLE on one host (migrated 2026-11-16, proven 2026-11-18) | Residual: Improv-WiFi-Library must compile/run on this pioarduino core and time-share the radio with our advertising. lora-relay proves the lib on the same core. Fallback: captive portal (SoftAP + DNSServer → `/setup`) |
| Discovery payloads rejected/ignored by HA (key names, `uniq_id` shape, availability arrays) | Open | The builder is one module pinned to the §7.2 schema (HA ≥ 2026.4 key set, no `object_id`); verify against the running HA's device registry at first build; keep all HA publishing behind that one seam |
| HA discovery lost if entities publish before `mqtt->begin()` | Open | Keep the §5 boot order; discovery publish is one function, called after connect and after each keying |
| Single-file Svelte bundle vs 896 KB LittleFS | Low | NFR-01 (≤ 100 KB gzipped); no heavy deps; vanilla svelte + vite + singlefile |
| NVS `put*` silently no-ops on read-only handle | Proven gotcha | All write paths use `readOnly=false` (CLAUDE.md §2) |
| Serial port swap (ACM0/ACM1) on power-cycle | Known | `ESP_SERIAL_PORT` override; `ls /dev/ttyACM*` |
| Single build breaks during bridge work (HA/web deps pulled in early) | Regression risk | NFR-03: build at the end of every phase; add each lib (HA, etc.) only in its own phase |
| Scanning vs control contention on one radio | Design | Single scan entry point + state machine; no overlapping BLE ops |
| HA group toggle = N-message burst vs single time-sliced radio | Design | §8 command queue: coalesce + paced drain (150 ms slots × R repeats); queue depth observable |

---

## 12. Implementation Phases

Build order is linear: each phase ends with a verifiable deliverable, and the
serial console path keeps working throughout (NFR-03).

### Phase 0: WiFi Provisioning (Improv BLE)

Improv first: a fresh board gets its WiFi from a phone before anything else
needs it. It stands on the already-landed `settings.*` scaffolding plus the
minimal half of the §5 boot flow it gates on (NVS load → STA attempt, bounded
timeout). The rest of the network shell (mDNS, httpd, web) is Phase 1.

| Task | Description |
|------|-------------|
| P0.1 | Add `jnthas/Improv-WiFi-Library`; `improvBLE.setDeviceInfo(...)` + `onImprovConnected` → creds to NVS → reboot (lora-relay pattern) |
| P0.2 | Runtime gating per §5: STA succeeds within timeout ⇒ Improv never starts; failed ⇒ LED red + Improv on |
| P0.3 | Re-entry: console `prov` (and later web "Provision via BLE") → clear saved WiFi creds → reboot |
| P0.4 | If the lib won't compile/coexist on this core: implement the captive-portal fallback (SoftAP `BRMesh-<last4>` + DNSServer wildcard → 302 `/setup`) instead |

**Deliverable:** A freshly-flashed board, with no serial, gets its WiFi from a
phone via Improv; a pre-provisioned board skips Improv entirely and reaches IDLE.

**Status: DONE** — Improv provisioning proven on hardware (Appendix A).

### Phase 1: Connectivity Foundation

Scaffolding already landed: `settings.*`, `statusled.*`, `light.h`,
`web/WebLog.*`, `web/ApiHttp.*`, the `MeshController`/`console` split, and the
NimBLE migration. This phase wires the network shell.

| Task | Description |
|------|-------------|
| P1.1 | `web/WebServer.{h,cpp}` — `esp_http_server` startup (lora-relay config), route table, gzipped-SPA handler (embedded fallback page until Phase 2), `/ws/log` handler, `g_webServer` handle |
| P1.2 | main.cpp boot completion per §5 (Phase 0 has the NVS → STA gating; add mDNS → httpd → loadLights()); replace the `server.handleClient()` model; wire log hooks into `WebLog` |
| P1.3 | Console config commands: `showcfg`, `set name/sid/pass/mqtt …` → NVS — the permanent serial fallback for config (primary path is the web Config page, Phase 2). `hostname [name]` already landed in Phase 0 (Tasmota-style SetHostName; NVS `name` + live netif apply) |
| P1.4 | platformio.ini: extend `build_src_filter` to include `web/*` (`settings.*`, `statusled.*` already build); Phase 3 needs **no new lib** — `esp-mqtt` is an ESP-IDF core component |
| P1.5 | Verify the single build still compiles and the proven console commands still work (NFR-03) |
| P1.6 | Firmware version tracking (lora-relay convention): `src/FirmwareVersion.h` is the single source of truth — `FIRMWARE_VERSION` + `kBoardModel`, one current-release description block, no changelog, bump here when a phase ships. Every surface that reports a version reads it: boot banner, Improv device info, `/api/status` (`fw` + `board`). Phase 2's `tools/build_spa.py` reads the same header (lora-relay regex) and bakes it into the SPA for the version-mismatch check |

**Verification:** flash → creds via Improv (Phase 0) or serial `set sid <ssid>` +
`set pass <pass>` → reboot → STA connects → from the host: `curl http://brmesh.local/api/status`
answers JSON, `GET /api/lights` lists the keyed bulb(s), and the serial console
(`list`, `on 1`) still works with the HTTP server running.

**Deliverable:** On the LAN, `<name>.local` serves a status page, `GET /api/status`
and `GET /api/lights` answer, the serial console works side-by-side with the HTTP
server, and the full config is settable from serial.

**Status: DONE (2026-11-19)** — flashed + verified from the host
(`curl http://brmesh/…`): `/api/status` (fw `1.1.0` + `board`), `/api/lights`
(light #1 with mac), `/` (embedded fallback), `/ws/log` (101 + log-tail
greeting), serial `list`/`showcfg` side-by-side with the HTTP server.
Partitions are the lora-relay shape (dual OTA + A/B spiffs, `spa_a` active).
**WiFi watchdog:** `loop()` owns mid-run STA reconnection (the boot flow runs
once and the driver's own background reconnect gives up) — rate-limited 30 s,
`LOGF` to the ring (`/ws/log`), LED `ERR`→`WIFI`, fresh `loadSettings()` per
attempt so a live console `set` is picked up. Verified to not misfire; live
drop-test pending (toggle the AP).

### Phase 2: Web UI (Svelte SPA)

| Task | Description |
|------|-------------|
| P2.1 | `frontend/` scaffold — Svelte + Vite + `vite-plugin-singlefile` + gzip; `tools/build_spa.py` pre-build action writing `index.html.gz` into LittleFS; embedded default page if the file is missing |
| P2.2 | Status/landing page — name, mode, IP, RSSI, MQTT link, lights count, fw, uptime (from `/api/status` + `/ws/log` status frames) |
| P2.3 | Lights page — per bulb: on/off, brightness slider, RGB picker, colour-temp slider (kelvin, P2.10), **rename** (inline edit, FR-13), **delete** (FR-12, with confirmation); believed state, live-updated over WS |
| P2.4 | Scan page — start with duration, live frame/identity count from the log stream, result list |
| P2.5 | Config page — name, **MQTT host/port/user/pass** (primary way to configure the broker — Phase 3 depends on it), "Provision via BLE", factory reset (confirmation); saving MQTT triggers an immediate connect attempt, no reboot |
| P2.6 | Console page — live log over `/ws/log`, raw-capture trigger (`POST /api/raw`), per-frame hex view |
| P2.7 | Hash routing (`#/status`, `#/lights`, …); WS event-driven updates, poll demoted to a reconnect safety net |
| P2.8 | **Believed state per bulb (FR-05):** the controller records the last intent — `{on, brightness, rgb, ct, colorMode}` — in `state`/`brightness`/`rgb`/`colorTemp`, persists it with the light in NVS (so it survives reboot), and returns it as `believed_state` on `GET /api/lights` (currently hardcoded `null`). The UI restores each swatch/slider from it — **fixes the "white box on refresh"** | Phase 2 follow-on (P2.8) |
| P2.9 | **State convergence re-transmit (FR-16):** a `loop()` cadence (default 10 s, tunable via console/`set`) re-sends each light's last known state so a bulb that missed a command converges. **Off stays off** (re-send `off`, never a colour frame); on → re-send the active colour (`rgb` or `ct`) + brightness. Skipped while scanning (radio time-sliced). Feeds the same `singleControl` seam; no direct BLE | Phase 2 follow-on (P2.9) |
| P2.10 | **Colour picker (FR-09, RGBW):** replace the `<input type="color">` rectangle with a BRMesh-app-style **colour wheel** (hue/saturation) for the RGB channel **plus a colour-temperature slider (kelvin, 2700–6000 K)** for the separate white LED (these bulbs are RGBW). Drive `rgb` and `ct` (kelvin) through the existing `POST /api/light?n=` | Phase 2 follow-on (P2.10) |
| P2.11 | **Leveled logger (FR-17):** replace raw `Serial.printf` with a leveled logger (`error`/`info`/`debug`) — settable level (console `set loglevel` + default) and a `t=<uptime>s` per-line prefix (so durations are visible), relayed to the `/ws/log` ring. Modeled on lora-relay's `Shared.h` logger (`LOG_ERROR/NORMAL/VERBOSE` + runtime `g_logLevel`) | Phase 2 follow-on (P2.11) |
| P2.12 | **UI-test coverage (NFR-07):** `frontend/e2e/smoke.mjs` exercises the new controls end-to-end against the board — colour wheel (drag → `believed_state.rgb`), white-LED CT slider (input → `believed_state.mode=ct`), and believed-state restore on refresh (White/CT stays active, no white box) — each as a single board-bound action. The idle-poll guarantee (the Lights page adds no per-second `/api/status` poller of its own; the App shell keeps its intentional 15 s meta poll) is a code-level invariant, not a network assertion. Every SPA change ships only green | Phase 2 follow-on (P2.12) |

**Deliverable:** Every operator task — provision, scan, key, control, configure,
diagnose — is doable from the browser; serial remains for firmware bring-up.

**Status — DONE (2026-11-19).** Shipped as one build (fw + spa 1.2.0), verified
on the board:
- Page order per the request: **Lights (landing), Config, Console, Info** —
  P2.4 scan is folded into the Lights page (one fewer tab, control-first).
- 26 kB gzipped single file (96 kB partition — 73% headroom).
- Full surface: light card (switch / brightness / RGB / rename / forget),
  scan panel (start · stop · live log tail), Config (device + MQTT +
  provision + factory-reset, masked secrets), Console (raw capture + live
  board log), Info (build match check + board KV).
- E2E: `frontend/e2e/smoke.mjs` (lora-relay's puppeteer harness, cached
  chrome-headless-shell) — **7/7 pass against the board** incl. one light
  toggle round-trip; screenshots in `e2e/shots/`.
- Two bugs caught by the e2e run: `function state()` shadowed Svelte 5's
  `$state` rune (→ `store_invalid_shape`, blank page), and `apiQuery()`
  inverted the `ESP_OK == 0` success check (→ `?n=` never parsed).

**Status — P2.8–P2.12 (added 2026-11-20): implemented; single build is green
(65.1 % flash). Board flash + e2e run pending the serial port / a physical
reset.** Follow-on asks layered on the Phase 2 surface:
- **P2.7 idle-poll fix (done):** the Lights page no longer polls `/api/status`
  on a 2.5 s timer — it polls only while `scanning`, and leans on the WS
  `logStream` for the live tail when idle (zero idle HTTP).
- **P2.8 believed state** (FR-05): the controller tracks each bulb's last intent
  (`on`/`brightness`/`rgb`/`ct`), persists it, and returns it as
  `believed_state` — the UI stops showing a white box on refresh.
- **P2.9 re-transmit** (FR-16): a ~10 s convergence loop re-sends the last known
  state so a bulb that missed a command (unpowered) catches up; off stays off.
- **P2.10 colour picker** (FR-09, RGBW): BRMesh-app-style colour wheel + a
  colour-temperature (white) slider, replacing the rectangle.
- **P2.11 leveled logger** (FR-17): `error`/`info`/`debug`, settable level,
  `t=<uptime>s` prefix (lora-relay `Shared.h` model).
- **P2.12 UI tests** (NFR-07): the e2e gate extended to the new controls; the
  ship gate is the e2e, not "it looked fine."

### Phase 3: HA Bridge (standard device-based discovery)

Discovery is HA's standard MQTT **device-based** schema (§7.2) — no hand-rolled
format and no HA library: retained JSON we publish against topics HA already
listens to.

**Status (implemented 2026-11-22, live-broker verification pending — the board
is gated on the broker being reachable; `mhost` empty ⇒ bridge disabled):**
lives in [HaBridge.\*](src/HaBridge.cpp) (one esp-mqtt client, all publishing
behind it). Identity: `ctrl_id = brmesh_<last 6 hex of MAC>` (broker-independent,
always available to the console/UI), bulb device `<ctrl_id>_l<n>`, MQTT client id
`brmesh_ctrl_<mac6>`. Hub name = NVS `mname` (empty → device `name`, shown as a
derived value on the Config page); hub `cu` is NOT configurable — always derived
at publish time as `http://<board IP>/` (a stored domain would not survive an IP
change and adds nothing at the firewall). Availability: ONE LWT (`brmesh/<ctrl_id>/status` "offline",
retained) + each bulb's `avty` is a topic **list** `[hub status, l<n>/availability]`
with `avty_mode: "all"`, so a single LWT takes the hub and every bulb offline at
once; bulbs forgotten while offline are queued and purged on reconnect. Light
state JSON is HA-native (`state`/`brightness` 0–255/`color`/`color_temp`
mireds/`color_mode`) — note this pulls P5.2's CT exposure forward, since Phase 2
already controls the white LED in kelvin. `T_TX`/`R` are compile constants
(`config.h`) for now, not NVS-tunable as P3.9 sketches — revisit only if field
tuning needs it.

| Task | Description |
|------|-------------|
| P3.1 | **`esp-mqtt`** (ESP-IDF core `esp_mqtt_client` — its own FreeRTOS task; no third-party lib, no `lib_deps` entry) on the NVS config (host/port/user/pass from the Config page), started in boot after httpd; client config: LWT `"offline"` (retained) on `brmesh/<ctrl_id>/status`, built-in backoff reconnect + session resume, buffer sized for the discovery payloads; birth `"online"` published on connect; `<ctrl_id>` = `brmesh_<MAC last 6 hex>`; **no MQTT creds ⇒ skip this step** — web + serial keep working |
| P3.2 | Discovery/state JSON builder (one small module, all HA publishing behind it): device-based payloads per §7.2 — abbreviated keys, `uniq_id` per component, `via_device`, `o` origin, `avty` + `avty_mode: "all"`, **no `object_id`**; publish retained QoS 1 only after the MQTT connect |
| P3.3 | Controller-hub device `homeassistant/device/<ctrl_id>/config`: `dev` with `ids`/`name`/`mf`/`mdl`/`sw` + **`cu` = SPA URL** (default `http://<board IP>/`, overridable as an NVS setting on the Config page); `cmps`: `Start scan` button (`cmd_t` → `brmesh/<ctrl_id>/scan`), sensors `scan state` / `lights registered` / `WiFi RSSI` / `MQTT link` / `queue depth` (diagnostic) — all reading one retained `brmesh/<ctrl_id>/state` JSON via per-sensor `val_tpl` |
| P3.4 | Bulb device per registered light `homeassistant/device/<ctrl_id>_l<n>/config` with `via_device: <ctrl_id>`; `cmps.main` = `light`: on/off + brightness (HA 0–255 ↔ wire 0–127) + RGB from the state JSON — believed state per §7 (CT exposure stays P5.2); subscribe `brmesh/<ctrl_id>/+/set`, parse the per-light command JSON → queue |
| P3.5 | Availability: per-bulb `brmesh/<ctrl_id>/l<n>/availability` + the hub `status` topic in every `avty` array; subscribe `homeassistant/status` → on `"online"`, full re-publish of all discovery + retained state (HA-restart recovery) |
| P3.6 | `brmesh/<ctrl_id>/scan` → single guarded entry point into `MeshController::startScan(seconds)` (rejects while scanning; LED blue) |
| P3.7 | Dynamic discovery: a bulb newly keyed during a scan publishes its device config + retained state immediately; removal (FR-12: `del <n>` / `DELETE /api/light/{n}`) publishes an **empty retained payload** to that config topic so HA purges the device |
| P3.8 | Rename path (FR-13): `PATCH /api/light/{n}` + console `rename` — save the name to NVS, re-publish the bulb's config with the new `dev.name` (ids/`uniq_id` unchanged, §7.2) so HA renames in place; verify in HA that the device tree under the hub is intact, entity_id unchanged, no orphan |
| P3.9 | Command queue (FR-14, §8): bounded FIFO + latest-wins coalescing + pacer (`T_TX`/`R` in NVS); **route every control source (MQTT `set`, web, serial, fan-out) through `enqueue()`** — nothing transmits directly; scan/queue exclusivity per §8.2; `queue_depth` in `/api/status`; prove with a 5-bulb group toggle: drain ~1 s, all bulbs follow, no radio contention |

**Deliverable:** With light #1 keyed, HA's device registry shows the **controller
hub** (diagnostics + `Start scan`, and a **Control UI link** that opens the Svelte
SPA) with the bulb **linked under it** (`via_device`): toggle/brightness/RGB from
HA flips the physical bulb, `Start scan` runs a scan from HA, HA restart re-binds
everything via `homeassistant/status`, and `list`/`/api/status` all agree.

### Phase 4: Add / Remove Lifecycle (proof with real bulbs)

The bridge must grow and shrink at runtime: a factory-new bulb found by a scan
appears in HA without a reboot, and a light removed in the UI disappears from HA.
Prove both directions with the on-hand factory-new stock (FR-12).

| Task | Description |
|------|-------------|
| P4.1 | **Add proof:** power up a factory-new bulb, start a scan (HA button / web / serial) → it is keyed, added to the NVS list, and its HA entity appears live (dynamic discovery) and is controllable — no reboot |
| P4.2 | **Multi-add proof:** repeat with a second bulb — light list grows, `lights registered` sensor tracks the count, each entity independent |
| P4.3 | **Remove proof:** delete a light from the web Lights page (confirmation) → `DELETE /api/light/{n}` → NVS saved + discovery unpublished → the HA entity disappears; console `del <n>` gives the same result |
| P4.4 | **Re-add:** reboot the bulb + `scan` restores its entry — the physical bulb keeps its key (removal deletes the record, not the keying) and still answers |
| P4.5 | **Persistence:** power-cycle after add + remove → the light list is exactly right: added bulbs remain, removed one does not come back |

**Deliverable:** A factory-new bulb: scan → keyed → in HA → controllable, all
without a reboot; delete in the UI → gone from HA; re-add restores it; and the
light list survives a power-cycle exactly as it was edited.


### Phase 5: Groups, Colour Temperature & Polish

| Task | Description |
|------|-------------|
| P5.1 | **Groups (two layers, §7.3):** virtual selections — `All lights` as a `light` component on the hub device (ESP-owned) + HA-owned scenes, both fanned out and batched through the §8 queue; **native room groups (L1, verify-first)** — capture + verify the `0x21` assign + type-3 frames on real bulbs, then assign bulbs to a room at keying and emit one group frame for whole-room ops (falls back to virtual fan-out until verified) |
| P5.2 | Colour-temperature control: map HA mireds to the bulb CT command set (calibrated against the bulb's actual range) |
| P5.3 | LittleFS upload endpoint (SPA replace without reflashing) |
| P5.4 | (Optional) firmware OTA with checksum verify + rollback, per lora-relay Phase 8 |

**Deliverable:** A finished bridge: N individually-controllable bulbs in HA
(add/remove proven), whole-room (native) + whole-scene (virtual) control per §7.3,
full colour control, and software-updatable UI/firmware.

---

## Appendix A — Proven Baseline

- POC **complete** on `70:e7:f2:72:70:e8` (light #1, key `09 90 1D D8`):
  detect → key → control (physically verified) → NVS persistence across reboots
  (a lost list entry recovers via reboot + `scan`).
- [protocol.cpp](src/protocol.cpp) is byte-identical to the proven community
  reference (protocol.md §3) — the active scan sees the bulbs and the
  detection/keying logic matches the real captured factory frame.
- NimBLE build re-proven 2026-11-18: detect → key → ON/OFF all verified.
- Factory key on fresh bulbs: `5E 36 7B C4` (protocol.md §2.6) — that is what
  makes SET_KEY possible.
- Field stock: 1 keyed bulb on Tasmota-controllable power, plus factory-new
  bulbs on hand — enough material for the Phase 4 add/remove lifecycle proofs
  (fresh bulbs advertise their factory identity on power-up; no app reset needed).

## Appendix B — Toolchain

- `pio` / `python`: `/home/rao/.platformio/penv/bin/pio`, `.../penv/bin/python`.
- Build + flash: `pio run -e waveshare_esp32_s3_zero --target upload`.
- Serial helpers: [tools/esp_serial.py](tools/esp_serial.py) (`boot`, `cmd`).
- Port: `/dev/ttyACM0` or `/dev/ttyACM1` — **can swap on power-cycle**
  (`ls /dev/ttyACM*`; override `ESP_SERIAL_PORT`). DTR/RTS does **not** reset
  this board — RESET button or USB power-cycle only.
- Power toggle for light #1: Tasmota `TASMOTA_URL` (`?cmnd=Power%20OFF|ON`) —
  **one request per state change, never loop/retry**; on a denial stop and report.
- Standing rules: no key extraction; no network/pin retry loops; `protocol.md`
  is the wire-format source of truth.

## Appendix C — Finding F-CT: op `0x72` = RGB **or** WW/CW white (unified)

**Date:** 2026-11-20. **Status: FIXED (2026-11-20); kelvin unit adopted 2026-11-22.** [MeshController::applyLight](src/MeshController.cpp) encodes the WW/CW crossfade from a kelvin value (`k` clamped 2700–6000 → `CW = (k−2700)*255/3300`, `WW = 255 − CW`) and both `colorTemp`/`rgb` send `d[2]=0xff` — the retired 14-bit split is gone. `ct` is now a kelvin label end-to-end (API, console, `believed_state`), stored as one 16-bit NVS value and split to the two white channels only at TX. The API/console/`believed_state`/e2e contract **did** change to kelvin (see §C.5). The fine kelvin→byte mid-curve (LUT) stays Phase 5 (P5.2).

### C.1 What we found
The on-hand bulbs are **RGBW**: a full RGB group **plus** a two-LED white (cool-white CW + warm-white WW), all driven by **one op `0x72`, one 8-byte payload, two mutually-exclusive modes**. That is **not** the single-axis 14-bit colour-temp the firmware currently encodes, and not a separate `colorTemp` op distinct from `rgb` — the app uses the same `0x72` command and switches modes by which channel group it sets. This is validated against the **app's own commands** (decoded under the app's key `35 30 32 32`; all frames checksum-close), captured 2026-11-20 on `70:e7…`.

Payload layout: `72 nn ff B R G WW CW` — `d[3]=B`, `d[4]=R`, `d[5]=G`, `d[6]=WW` (warm-white), `d[7]=CW` (cool-white); RGB mode zeroes the white pair, white mode zeroes the RGB triple.

**White (colour-temp) mode — `B=R=G=0`, `WW = 255 − CW` (linear crossfade):** channel order `d[6]=warm, d[7]=cool` is hardware-verified on the physical bulb (2026-11-20) — the original kelvin↔byte association was recorded reversed.

| app colour temp | data bytes (d[0..7]) | d[6] WW (warm) | d[7] CW (cool) |
|---|---|---|---|
| 2700 K (warm end) | `72 nn ff 00 00 00 ff 00` | `0xff` (WW full) | `0x00` |
| ~4100 K (middle) | `72 nn ff 00 00 00 87 78` | `0x87` | `0x78` |
| 6000 K (cool end) | `72 nn ff 00 00 00 00 ff` | `0x00` | `0xff` (CW full) |

**RGB mode — `CW=WW=0`, byte order `d[3]=B, d[4]=R, d[5]=G` (app-validated):**

| colour | d[3] B | d[4] R | d[5] G |
|---|---|---|---|
| green | 00 | 00 | ff |
| blue | ff | 00 | 00 |
| magenta | ff | ff | 00 |
| cyan | ff | 00 | ff |
| lime (DIY) | 00 | 62 | ff |

Secondary (minor): the app sends `d[2]` = `0xff` (on|max-brightness) in **every** frame (RGB and white) while the shipped code sends `d[2]` = `0x80` — a brightness-bits difference, not the core issue (the exact role of `d[2]` is still inferred). Full detail + open items: [protocol.md](protocol.md) §2.5b.

### C.2 Why the shipped encoding is wrong
[MeshController::colorTemp](src/MeshController.cpp) encodes `d[6] = t & 127; d[7] = (t >> 8) & 127` — a **single 14-bit value split across the two bytes**. That (a) 7-bit-masks both channels (max `0x7F`), so it **cannot express either app endpoint** (both need a full `0xff` channel), and (b) treats the two channels as **one axis** when they are a **crossfade** (`WW = 255 − CW`). Net effect: our CT slider never lands on the app's colour temperatures. Two related mismatches: the shipped `colorTemp`/`rgb` both send `d[2] = 0x80` where the app sends `0xff`; and `rgb()` is otherwise byte-order correct (`d[3]=B, d[4]=R, d[5]=G`) but was never validated against the air — it now is.

### C.3 Affected surfaces (all assume the retired single-axis model)
| surface | what it assumes | location |
|---|---|---|
| `colorTemp(n, uint16_t t)` — the 14-bit split | single-axis CT | [src/MeshController.cpp:99-108](src/MeshController.cpp#L99) |
| `colorTemp(uint8_t, uint16_t)` signature | `uint16_t` kelvin parameter | [src/MeshController.h:67](src/MeshController.h#L67) |
| `ct <0-32767>` command + help text | 15-bit CT range | [src/console.cpp:22](src/console.cpp#L22), [121-125](src/console.cpp#L121) |
| `CT_MAX = 16383`, slider 0–100 → CT_MAX | 14-bit ceiling | [frontend/src/lib/ColorPicker.svelte:21](frontend/src/lib/ColorPicker.svelte#L21) |
| `{"ct":N}` passthrough (no rescale) | CT integer | [src/web/ApiLights.cpp:104-105](src/web/ApiLights.cpp#L104) |
| `believed_state.ct` / `colorMode=2` | single CT value | [src/light.h](src/light.h) |
| CT-slider e2e case | single-axis restore | [frontend/e2e/smoke.mjs](frontend/e2e/smoke.mjs) |
| `colour temp` table row | retired encoding | [protocol.md §2.5](protocol.md) |

> Note (2026-11-22): the kelvin unit (2700–6000 K) now supersedes these surfaces — `ct` is a kelvin label end-to-end and stored as one 16-bit NVS value. The table above is the 2026-11-20 as-found snapshot.

### C.4 Reclaim status (NOT a blocker — already ours)
`70:e7…` is **already reclaimed to our key `09 90 1D D8`** and in our control. The Nov-20 capture that decoded under the app's key `35 30 32 32` was the **app's own broadcast, sent under the app's key** — that is what makes it decode under that key; it proves nothing about which key the bulb holds. The bulb carries *our* key, so the app's continued broadcasts are **inert** (the bulb decrypts the data region under our key, the app's frames fail the §2.1 checksum and are ignored). So **our board can drive `70:e7…` directly — no reclaim step is needed** before verifying the new white-channel encoding on hardware. (The app-key rotation note in protocol.md §2.7 still stands as general guidance for any bulb the app *has* claimed and we have *not* yet reclaimed.)

### C.5 Open design decisions (choose before touching code)
**Now settled by the captures:** RGB and white are **mutually exclusive** — one `0x72` command carries either the RGB triple **or** the CW/WW pair, never both. So the model is two modes of one op, not two independent values that coexist. That removes the "coexistence" branch of the decision and confirms the UI needs a **mode toggle** (colour wheel ↔ white slider), matching the app.

**Resolved (2026-11-20 white-channel fix; kelvin unit adopted 2026-11-22):**
1. **Slider semantics — kelvin.** `colorTemp(n, k)` takes a kelvin value (`k` 2700 warm → 6000 cool) and maps it linearly **at TX** to `CW = (k−2700)*255/3300`, `WW = 255 − CW`; `k` is stored as one 16-bit `ct` and split to the two white channels only in `applyLight`. 
2. **Kelvin range — the control unit.** 2700–6000 K is now the API/console/`believed_state` unit (the app's display label promoted to the control field); it is **not** on the air — only the 0–255 WW/CW pair is (protocol.md §2.5b).
3. **Parameter model — kelvin end-to-end.** `colorTemp(n, k)`, console `ct <2700-6000>`, `{"ct":<kelvin>}`, and `believed_state.ct` are all kelvin now; `believed_state` keeps its `mode` (colour | white) flag + `ct`/`rgb` values. The UI slider (P2.10) sends/reads kelvin. The fine kelvin→byte mid-curve (LUT) stays Phase 5 (P5.2).
4. **`d[2]` = `0xff`** — adopted in both `colorTemp` and `rgb` (matches the air).
5. **Reclaim first — moot.** `70:e7…` is already on our key (C.4); verify the new encoding on it directly.
