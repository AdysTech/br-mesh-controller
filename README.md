# BRMesh → Home Assistant Controller (ESP32-S3)

Native C++ firmware for an **ESP32-S3** that bridges **BroadLink "FastCon" BLE
broadcast lights** (the BRGD/BRMesh product line — Android app
`com.brgd.brblmesh`) into **Home Assistant** over MQTT.

> This is an **independent interoperability implementation** of a vendor protocol.
> It is **not affiliated with or endorsed by BroadLink**.

## What these lights actually are

These bulbs are **not real BLE Mesh**. They are controlled entirely by **BLE
advertising / broadcast frames** — BroadLink's **FastCon** air protocol. There is
**no pairing, no connection, and no acknowledgement**: you broadcast a command
frame and the bulb simply listens.

**Naming (kept distinct in code and docs):**
- **FastCon** — the vendor's (BroadLink's) patented BLE broadcast air technology.
- **BRMesh** — the product line of these bulbs.
- **BRMesh Controller** — this firmware.

## How this project controls them

The controller is a **key originator**:
- it **generates its own 4-byte key**, and
- **writes that key onto factory-keyed bulbs** (`SET_KEY`).

The factory/default key ships **in the clear** inside the bulb's own advertisement
— that is the enabler that makes `SET_KEY` possible. The controller deliberately
**does not extract any key from the phone app**; it creates and installs its own.
Each light is exposed **individually** (no group APIs), and the key + light list
are **persisted in NVS**, so a reboot does not re-key.

## Status

**POC proven** (2026-11-16): detect → key → control → persist, all demonstrated on
real hardware — discovery, `SET_KEY`, and state / brightness / colour-temp / RGB
control confirmed on a bulb. Build, flash, and native-USB serial all work.

## How the protocol was obtained

Reverse-engineered from **manual over-the-air captures** (broadcast frames — nothing
here requires a connection): the bulb's factory-identity advertisement, the phone
app's `WAKE` broadcast, and the post-key identity frame. The frame packing is
derived from, and cross-checked against, two prior community reverse-engineerings —
see [Credit](#credit-and-attribution). The full byte-level layout and the
discovery/keying flow are in [CLAUDE.md](CLAUDE.md) (§0, §4, §5).

## Building & flashing

Uses [PlatformIO](https://platformio.org):

```bash
pio run -e <env>            # build
pio run -e <env> -t upload  # flash
```

- Board: **ESP32-S3** (4 MB flash / 2 MB PSRAM), **native USB serial** (no UART
  bridge) — see [platformio.ini](platformio.ini).
- Serial helpers (discovery / raw-capture drivers) live in [tools/](tools/) and are
  fully env-driven (`ESP_SERIAL_PORT`, `TASMOTA_URL`) — no machine-specific values
  are checked in.

## Repository layout

| Path | What |
|---|---|
| `src/main.cpp` | device, discovery, keying, serial console, (HA bridge) |
| `src/protocol.{h,cpp}` | the FastCon codec — **stack-agnostic**, the proven byte-level core |
| `src/config.h` | WiFi / MQTT / LED / scan duration / `ENABLE_WIFI` |
| `tools/*.py` | pyserial serial + discovery capture helpers |
| `CLAUDE.md` | the full deep-dive: naming, provenance, protocol, status, toolchain, rules |

## Credit & attribution

The frame-packing in `src/protocol.{h,cpp}` is derived from the following
community reverse-engineerings of the same FastCon protocol, which are thanked and
credited in [CLAUDE.md §8](CLAUDE.md):

- **`dsclee1/BRmesh-esp32-mqtt`** — <https://github.com/dsclee1/BRmesh-esp32-mqtt>
  (ESP32 + MQTT). The ground truth our frame packing mirrors byte-for-byte.
- **`scross01/esphome-fastcon`** — <https://github.com/scross01/esphome-fastcon>
  (esphome component). Independent implementation of the same keying model; a useful
  cross-check.

Please see each project's own repository for its license terms.

## License

Released under the **MIT License** — see [LICENSE](LICENSE). Copyright AdysTech.

The frame-packing in `src/protocol.{h,cpp}` is derived from the community
projects credited above, and is **attributed accordingly**; please also observe
their respective terms (`scross01/esphome-fastcon` is MIT;
`dsclee1/BRmesh-esp32-mqtt` ships no explicit license).


Co-Authored-By: Self hosted Qwen 3.8:27b via Claude Code as harness
