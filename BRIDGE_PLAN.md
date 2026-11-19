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
| FR-08 | The serial console (POC command set: `list`, `sel`, `on/off/ct/rgb`, `scan`, `raw`, `readd`…) shall keep working in bridge mode | Mandatory |
| FR-09 | Colour-temperature control (bulbs advertise type `2A A8`) | Later (Phase 5) |
| FR-10 | "All lights" **virtual selection** (ESP-owned fan-out entity, every keyed bulb) — §7.3 L2 | Later (Phase 5) |
| FR-15 | **Native room groups (§7.3 L1):** a bulb is assigned to a physical room at keying; whole-room command = one FastCon group frame. **Verify-first** on the `0x21` assign + type-3 group frames (not yet in protocol.md); until verified, a room group is a virtual selection (L2) | Later (Phase 5, gated on verification) |
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
 │  • HA lib: HALight per bulb, button, sensors           │
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
| HA | `dawidchyrzynski/home-assistant-integration` (HADevice/HAMqtt/HALight) + hand-rolled discovery for button/sensors | Per-bulb `HALight`; controller entities hand-rolled. Currently commented out in [platformio.ini](platformio.ini). |
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
| **NVS** `brmesh` | name; wifi ssid/pass; mqtt host/port/user/pass; 4-byte mesh key; light list (id, **name**, mac, type, features) | Fast read/write; **write paths must open with `readOnly=false`** (proven gotcha, CLAUDE.md §2) |
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
   │                     MQTT creds in NVS? ── no ──▶ skip HA (web+serial only)
   │                        │ yes
   │                        ▼
   │                     MQTT connect → HA entities publish
   │                        (discovery AFTER mqtt->begin — order is load-bearing)
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

| Entity | Type | Source |
|--------|------|--------|
| One light per keyed bulb (user-renamable, FR-13) | `HALight` (RGB + brightness first; CT in Phase 5) | home-assistant-integration lib |
| **Room group** (physical, e.g. `living_room`) — §7.3 L1 | `HALight` (ESP-owned) | native group frame = **1 TX** once `0x21`/type-3 verified; else L2 batched fan-out |
| **Virtual selection** (e.g. `All lights`, FR-10) — §7.3 L2 | `HALight` (ESP-owned) | fan-out through the §8 queue (Phase 5) |
| **Virtual selection** (e.g. `TV rim`) — §7.3 L2 | HA `group`/`scene` (HA-owned) | HA fans out to member lights → §8 queue |
| `Start scan` | HA **button** | hand-rolled discovery |
| `Scan state` (IDLE/SCANNING/KEYING) | HA sensor | hand-rolled discovery |
| `Lights registered` | HA sensor | hand-rolled discovery |
| `WiFi RSSI` / `MQTT link` | HA sensors | hand-rolled discovery |

> **A group is a separate entity, not a field on the light.** The individual light
> entity carries **no `group_id`** — a group is either its own ESP entity (room /
> `All lights`) or an HA-side composition (scene). The native `group_id` is
> ESP/bulb-internal state that powers the 1-frame room path (§7.3); it is never a
> payload HA relies on.

### 7.2 Topic Layout

| Direction | Topic | Payload |
|-----------|-------|---------|
| HA → ESP | `brmesh/<name>/light/<n>/cmnd/on` | `ON`/`OFF` |
| HA → ESP | `brmesh/<name>/light/<n>/cmnd/brightness` | `0`–`255` (scaled to 0–127) |
| HA → ESP | `brmesh/<name>/light/<n>/cmnd/rgb` | `[r,g,b]` |
| HA → ESP | `brmesh/<name>/cmnd/scan` | `START` |
| HA → ESP | `brmesh/<name>/group/<gid>/cmnd/<op>` | `on`/`brightness`/`rgb` — native group frame (L1) or fan-out (L2), §7.3 |
| ESP → HA | `brmesh/<name>/light/<n>/state` | `{"on":true,"brightness":120,"rgb":[..]}` (retained, believed) |
| ESP → HA | `brmesh/<name>/stat/<sensor>` | retained value updates |
| Discovery | `homeassistant/light/brmesh_<n>/config`, `homeassistant/light/brmesh_room_<gid>/config`, `homeassistant/button|sensor/brmesh_<name>_<thing>/config` | published on MQTT connect, on keying, and on rename (lights); object ids stable per §7.2 — room = `brmesh_room_<gid>` (stable id, never the name) |

**Rules:**
- Believed state: entity state = last command sent; no readback exists.
- Discovery is published **after** `mqtt->begin()` and again whenever a new bulb
  is keyed during a scan (dynamic discovery).
- Bulbs are broadcast-only: a command is "sent" not "confirmed" — HA optimistically
  reflects the command, the ESP logs the send.
- **Stable identity (zigbee2mqtt model):** the discovery **object id** is
  `brmesh_<n>` — derived from the light's number, *never* from its name. The name is
  only the `name` field in the discovery payload (zigbee2mqtt `friendly_name`).
- **Rename (FR-13):** `rename <n> <text>` (console) / `PATCH /api/light/{n}` saves the
  name to NVS and re-publishes that light's discovery config with the new `name`. HA
  keeps the same entity (object id unchanged) and updates its name in place — no
  orphan entity, no re-pairing. `lights registered` sensors/button are unaffected.
- **Controller rename:** `brmesh/<name>/…` cmnd/state topics embed the controller
  name — renaming the controller (Config page) re-publishes **all** discovery +
  retained state so HA rebinds to the new topics.
- **Removal (FR-12):** `del <n>` (console) / `DELETE /api/light/{n}` removes the NVS
  entry, saves, and unpublishes that light's discovery topic (empty payload) so the HA
  entity disappears. Note: the physical bulb keeps its key — it still obeys our
  broadcast — it is simply no longer represented; re-addable any time via
  `readd <mac> [n]`.

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
- **ESP-owned** — a `HALight` on the controller (e.g. the `All lights` fan-out,
  FR-10). Membership in NVS; the controller expands one command into N queue
  items, batched.
- **HA-owned** — a HA `group`/`scene` composed over the individual light entities
  in the HA UI (e.g. `TV rim`). HA fans out → N cmnd messages → the §8 queue.
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
sources (HA cmnd topics, fan-out, web POSTs, serial) converge on one queue that is
the **sole owner of control airtime** (FR-14).

### 8.1 Problem

An HA group light toggled off by the user produces **N MQTT cmnd messages within
milliseconds** (one per member). The radio is single and time-sliced; an
unsynchronized burst would collide with itself (and with a scan). So bursts must
be serialized and paced.

### 8.2 Design

| Aspect | Decision |
|--------|----------|
| Structure | Bounded FIFO (default **64 items**) in `MeshController`; each item = `{light n, op}` where op ∈ {on/off, brightness, rgb, ct} |
| Sources | MQTT cmnd handlers, fan-out expansion (1 "All lights" msg → N items), `POST /api/light/{n}`, serial console — **all enqueue, none transmit directly** |
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
  (`scan 12` + early Tasmota power-cycle), driver [tools/scan_catch.py](tools/scan_catch.py).
- NimBLE drops the advertiser when a scan starts → everything time-slices:
  WAKE/SET_KEY advertise burst → scan burst loop.
- `wantDuplicates=true` matters: bulbs interleave bare beacons with identity frames.
- Recovery without air traffic: `readd <mac> [num]` (NVS list repair only).
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
| `HALight`/`HAMqtt` API signatures differ from assumptions | Open | Verify against the fetched lib at first build; keep all HA calls behind one `MeshController` seam |
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
| P1.4 | platformio.ini: extend `build_src_filter` to include `web/*` (`settings.*`, `statusled.*` already build); HA lib dep stays commented until Phase 3 |
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
| P2.3 | Lights page — per bulb: on/off, brightness slider, RGB picker, CT placeholder (Phase 5), **rename** (inline edit, FR-13), **delete** (FR-12, with confirmation); believed state, live-updated over WS |
| P2.4 | Scan page — start with duration, live frame/identity count from the log stream, result list |
| P2.5 | Config page — name, **MQTT host/port/user/pass** (primary way to configure the broker — Phase 3 depends on it), "Provision via BLE", factory reset (confirmation); saving MQTT triggers an immediate connect attempt, no reboot |
| P2.6 | Console page — live log over `/ws/log`, raw-capture trigger (`POST /api/raw`), per-frame hex view |
| P2.7 | Hash routing (`#/status`, `#/lights`, …); WS event-driven updates, poll demoted to a reconnect safety net |

**Deliverable:** Every operator task — provision, scan, key, control, configure,
diagnose — is doable from the browser; serial remains for firmware bring-up.


### Phase 3: HA Bridge

| Task | Description |
|------|-------------|
| P3.1 | `HADevice` + `HAMqtt` on NVS config (host/port/user/pass from the Phase 2 config page), connect in boot after httpd; topic root `brmesh/<name>/` per §7.2; **no MQTT creds ⇒ skip this step** — web + serial keep working |
| P3.2 | `HALight` per registered bulb: on/off, brightness (0–127), RGB — believed state per §7 |
| P3.3 | Hand-rolled HA discovery: button `Start scan`, sensors `scan state`, `lights registered`, `WiFi RSSI`, `MQTT link`; discovery publish after `mqtt->begin()` |
| P3.4 | `cmnd/scan` → single guarded entry point into `MeshController::startScan(seconds)` (rejects while scanning; LED blue) |
| P3.5 | Dynamic discovery: a bulb newly keyed during a scan publishes its light-discovery + state immediately |
| P3.6 | State sync: believed state updates pushed to HA retained topics on every send (console, web, or HA-originated); MQTT reconnect (broker blip) re-publishes all discovery + retained state |
| P3.7 | Rename path (FR-13): `PATCH /api/light/{n}` + console `rename` — save name to NVS, re-publish discovery with stable object id `brmesh_<n>` (see §7.2) so HA renames the entity in place; verify in HA that the entity_id is unchanged and no orphan appears |
| P3.8 | Command queue (FR-14, §8): bounded FIFO + latest-wins coalescing + pacer (`T_TX`/`R` in NVS); **route every control source (MQTT cmnd, web, serial, fan-out) through `enqueue()`** — nothing transmits directly; scan/queue exclusivity per §8.2; `queue_depth` in `/api/status`; prove with a 5-bulb group toggle: drain ~1 s, all bulbs follow, no radio contention |

**Deliverable:** With light #1 keyed, HA shows the light + controller entities:
toggle/brightness/RGB from HA flips the physical bulb, `Start scan` runs a scan
from HA, and `list`/`/api/status` all agree.

### Phase 4: Add / Remove Lifecycle (proof with real bulbs)

The bridge must grow and shrink at runtime: a factory-new bulb found by a scan
appears in HA without a reboot, and a light removed in the UI disappears from HA.
Prove both directions with the on-hand factory-new stock (FR-12).

| Task | Description |
|------|-------------|
| P4.1 | **Add proof:** power up a factory-new bulb, start a scan (HA button / web / serial) → it is keyed, added to the NVS list, and its HA entity appears live (dynamic discovery) and is controllable — no reboot |
| P4.2 | **Multi-add proof:** repeat with a second bulb — light list grows, `lights registered` sensor tracks the count, each entity independent |
| P4.3 | **Remove proof:** delete a light from the web Lights page (confirmation) → `DELETE /api/light/{n}` → NVS saved + discovery unpublished → the HA entity disappears; console `del <n>` gives the same result |
| P4.4 | **Re-add:** `readd <mac> [n]` restores an entry — the physical bulb keeps its key (removal deletes the record, not the keying) and still answers |
| P4.5 | **Persistence:** power-cycle after add + remove → the light list is exactly right: added bulbs remain, removed one does not come back |

**Deliverable:** A factory-new bulb: scan → keyed → in HA → controllable, all
without a reboot; delete in the UI → gone from HA; re-add restores it; and the
light list survives a power-cycle exactly as it was edited.


### Phase 5: Groups, Colour Temperature & Polish

| Task | Description |
|------|-------------|
| P5.1 | **Groups (two layers, §7.3):** virtual selections — `All lights` `HALight` (ESP-owned) + HA-owned scenes, both fanned out and batched through the §8 queue; **native room groups (L1, verify-first)** — capture + verify the `0x21` assign + type-3 frames on real bulbs, then assign bulbs to a room at keying and emit one group frame for whole-room ops (falls back to virtual fan-out until verified) |
| P5.2 | Colour-temperature control: map HA mireds to the bulb CT command set (calibrated against the bulb's actual range) |
| P5.3 | LittleFS upload endpoint (SPA replace without reflashing) |
| P5.4 | (Optional) firmware OTA with checksum verify + rollback, per lora-relay Phase 8 |

**Deliverable:** A finished bridge: N individually-controllable bulbs in HA
(add/remove proven), whole-room (native) + whole-scene (virtual) control per §7.3,
full colour control, and software-updatable UI/firmware.

---

## Appendix A — Proven Baseline

- POC **complete** on `70:e7:f2:72:70:e8` (light #1, key `09 90 1D D8`):
  detect → key → control (physically verified) → NVS persistence across reboots,
  plus `readd` list recovery.
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
- Serial helpers: [tools/esp_serial.py](tools/esp_serial.py) (`boot`, `cmd`);
  catch driver: [tools/scan_catch.py](tools/scan_catch.py) (proven).
- Port: `/dev/ttyACM0` or `/dev/ttyACM1` — **can swap on power-cycle**
  (`ls /dev/ttyACM*`; override `ESP_SERIAL_PORT`). DTR/RTS does **not** reset
  this board — RESET button or USB power-cycle only.
- Power toggle for light #1: Tasmota `TASMOTA_URL` (`?cmnd=Power%20OFF|ON`) —
  **one request per state change, never loop/retry**; on a denial stop and report.
- Standing rules: no key extraction; no network/pin retry loops; `protocol.md`
  is the wire-format source of truth.
