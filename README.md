# BRMesh → Home Assistant Controller (ESP32-S3)

Native C++ firmware for an **ESP32-S3** that bridges **BroadLink "FastCon" BLE broadcast lights** (the BRGD/BRMesh product line — Android app `com.brgd.brblmesh`) into **Home Assistant** over MQTT.

> This is an **independent interoperability implementation** of a vendor protocol. It is **not affiliated with or endorsed by BroadLink**.

## What these lights actually are

These bulbs are **not real BLE Mesh**. They are controlled entirely by **BLE advertising / broadcast frames** — BroadLink's **FastCon** air protocol. There is **no pairing, no connection, and no acknowledgement**: you broadcast a command frame and the bulb simply listens. Bulbs also **re-broadcast** a command they hear, which is what carries one controller's broadcast across the mesh.

**Naming (kept distinct in code and docs):**
- **FastCon** — BroadLink's patented BLE broadcast air technology.
- **BRMesh** — the product line of these bulbs.
- **BRMesh Controller** — this firmware.

## What it does

The controller is the **key originator**: it **generates its own 4-byte key** and **writes that key onto factory-keyed bulbs** (`SET_KEY`). The factory/default key ships **in the clear** inside the bulb's own advertisement — that is the enabler that makes `SET_KEY` possible. The controller deliberately **does not extract any key from the phone app**; it creates and installs its own. Each light is exposed **individually** (no group APIs), and the key + light list + settings are **persisted in NVS**, so a reboot does not re-key.

Implemented today (one build, one firmware):
- **WiFi** — STA mode with **Improv BLE** provisioning (boots into provisioning when unprovisioned or when the link keeps failing).
- **Web shell** — a REST API (`/api/*`), a live log WebSocket (`/ws/log`), and a **Svelte SPA** (Lights · Config · Console · Info) served from LittleFS.
- **Home Assistant bridge** — standard MQTT **device-based discovery**: one hub device (diagnostics + a "Start scan" button) and one **light entity per bulb** (on/off, brightness, RGB, colour-temp), all as retained QoS1 JSON over the esp-mqtt client.
- **Serial console** (115200) — discovery, per-light control, config, provisioning, and raw-capture debugging.
- **Portable backup / restore** — the board's full state (settings + mesh key + light list) as a JSON snapshot.

## Requirements

**Hardware** — any **ESP32-S3** with **4 MB flash** and **2 MB PSRAM**, using **native USB serial** (CDC-on-boot; no UART bridge). The checked-in build targets a Waveshare ESP32-S3 Zero as the reference board, but the code is board-agnostic for any equivalent ESP32-S3.

**Software** — [PlatformIO](https://platformio.org) with the `pioarduino` / Arduino-ESP32 core, C++17. The Svelte SPA needs Node.js to build (`npm`); everything else is standard C/C++.

## Building & flashing

```bash
pio run              # build the firmware (also builds the SPA into LittleFS)
pio run -t upload    # flash the firmware (flashes the SPA first if it's newer)
pio run -t uploadfs  # flash just the SPA (LittleFS) slot
```

Serial / discovery / capture helpers live in [tools/](tools/) and are fully env-driven (`ESP_SERIAL_PORT`, `CAP_CMD`, `CAP_SEC`) — no machine-specific values are checked in. See [CLAUDE.md](CLAUDE.md) §5 for the exact recipe.

## Repository layout

| Path | What |
|---|---|
| `src/main.cpp` | thin app shell — setup/loop, the boot gate (NVS → STA → Improv) |
| `src/MeshController.{h,cpp}` | the engine — key + lights, BLE advertise/scan, discovery/keying, NVS |
| `src/protocol.{h,cpp}` | the FastCon codec — stack-agnostic, the proven byte-level core |
| `src/HaBridge.{h,cpp}` | the HA bridge — one MQTT client, all device-discovery/state publishing |
| `src/web/` | the web shell — httpd routes, SPA handler, `/ws/log`, the REST handlers |
| `src/console.{h,cpp}` | the serial console driver |
| `src/settings.{h,cpp}` | NVS-backed settings (creds, name, MQTT, scan window) |
| `src/statusled.{h,cpp}` | the WS2812 status LED |
| `frontend/` | the Svelte 5 SPA (Lights · Config · Console · Info) + its e2e smoke |
| `tools/` | pyserial serial + discovery/capture helpers (env-driven) |
| `CLAUDE.md` | the deep-dive: naming, provenance, protocol, toolchain, rules |

## How the protocol was obtained

Reverse-engineered from **manual over-the-air captures** (broadcast frames — nothing here requires a connection): the bulb's factory-identity advertisement, the phone app's `WAKE` broadcast, and the post-key identity frame. The byte-level layout, framing, and discovery/keying flow are documented in [protocol.md](protocol.md) and [CLAUDE.md](CLAUDE.md).

## Credit & attribution

The frame-packing in `src/protocol.{h,cpp}` is derived from the following community reverse-engineerings of the same FastCon protocol:

- **`dsclee1/BRmesh-esp32-mqtt`** — <https://github.com/dsclee1/BRmesh-esp32-mqtt> (ESP32 + MQTT). The ground truth our frame packing mirrors byte-for-byte.
- **`scross01/esphome-fastcon`** — <https://github.com/scross01/esphome-fastcon> (esphome component). An independent implementation of the same keying model; a useful cross-check.

Please see each project's own repository for its license terms.

## License

Released under the **MIT License** — see [LICENSE](LICENSE). Copyright AdysTech.

The frame-packing in `src/protocol.{h,cpp}` is derived from the community projects credited above and is **attributed accordingly**; please also observe their respective terms (`scross01/esphome-fastcon` is MIT; `dsclee1/BRmesh-esp32-mqtt` ships no explicit license).

---

Co-Authored-By: Self hosted Qwen 3.8:27b via Claude Code as harness
