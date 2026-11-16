# CLAUDE.md — BRMesh → Home Assistant controller (ESP32-S3)

Native C++ firmware for an **ESP32-S3** that bridges **BRGD / BRMesh BLE lights**
(the Android app is `com.brgd.brblmesh`) into **Home Assistant** over MQTT.

Read this top-to-bottom before touching the code. It captures the naming, the
reverse-engineered protocol, the proven-correct parts, and exactly how to
build/flash/test.

---

## 0. Naming & provenance (read this first)

Three distinct things — do not conflate them:

- **FastCon (BLE)** — a **patented broadcast technology developed by BroadLink**
  (the vendor behind these bulbs). It is **not real BLE Mesh**: no pairing, no
  connection, no acknowledgements. Commands travel as **BLE advertising/broadcast
  frames** and the bulbs simply listen. "FastCon" is the vendor's name for that
  underlying air interface — community reference repos inherited it from the vendor
  code (e.g. `package_ble_fastcon_body`, `DEFAULT_BLE_FASTCON_ADDRESS`).
- **BRMesh** — the **product line** of these bulbs (the BRGD/BRMesh app ecosystem).
- **This project (BRMesh Controller)** — an ESP32-S3 that **reverse-engineered the
  FastCon broadcast protocol** and bridges the bulbs into Home Assistant over MQTT.
  It is a **key originator**: the ESP generates its own 4-byte key and writes it
  onto factory-keyed bulbs (we deliberately do NOT extract any key from the app).

Role map (keep it this way in code, docs, HA entity names):

| Layer | Name | Where it appears |
|---|---|---|
| Vendor BLE broadcast tech (patented) | **FastCon** | the air protocol, `protocol.{h,cpp}` comments, §4 |
| Bulb product line | **BRMesh** | HA device names, serial banner, repo name, NVS namespace |
| This firmware | **BRMesh Controller** | this repo |

This is an independent interoperability implementation of a vendor protocol; it is
not affiliated with or endorsed by BroadLink.

### How the protocol was actually obtained

The FastCon framing in §4 was reverse-engineered from **manual over-the-air
captures** (broadcast frames — nothing here requires a connection), not from any
SDK or vendor documentation:

1. **Bulb factory-identity frames** — the 18-byte manufacturer advertisements
   captured from all three bulbs (`70:e7…`, `2a:e6…`, `0b:ed…`) in factory state.
   Layout in §4.6. These exposed the MAC-reversed identity, the type code, and —
   the key enabler — the **factory key `5E 36 7B C4` sitting in the clear** in the
   advertisement, which is what makes SET_KEY possible.
2. **The phone app's WAKE broadcast** — the 24-byte WAKE payload the BRGD app
   emits, captured live during use; replayed by the `wakephone` console command
   (debug path, not the primary one).
3. **Keyed identity frames** — after SET_KEY the bulb re-advertises the same
   18-byte shape but with **our** key at `[14..17]`; `AddLightCallback` decrypts
   the assigned number with our key. This confirmation loop is what made keying
   observable and verifiable.

Credit where due — two independent community reverse-engineerings of the same
FastCon air protocol preceded and informed ours (full detail in §8):

- **`dsclee1/BRmesh-esp32-mqtt`** (ESP32 + MQTT, 2024) supplied the frame-packing
  math (`do_generate_command` / `get_rf_payload` / `whiteningEncode`), which our
  `protocol.cpp` mirrors **byte-for-byte**.
- **`scross01/esphome-fastcon`** (esphome component, active 2026) independently
  documents the same `controller_id` / `mesh_key` keying model we implement, and
  is a useful cross-check on the frame layout.

But **neither reference was in a directly workable condition for our hardware** —
the parts that actually work here are ours: the concurrent WAKE+scan discovery
recipe (§3/§5), factory-frame detection and keying, NVS persistence, and the
serial console.

---

## 1. The core model (do NOT deviate)

- These bulbs are **NOT real BLE Mesh**. They are controlled entirely by **BLE
  advertising / broadcast frames** (a protocol called **FastCon**). There is **no
  pairing, no connection, no acknowledgements** — you *broadcast* a command frame
  on the air and the bulb listens.
- The ESP is the **KEY ORIGINATOR**: it **generates its own 4-byte key**, and writes
  that key **onto factory-keyed bulbs** (SET_KEY). We deliberately do **NOT** extract
  any key from the Android app (the user explicitly wants to avoid that).
- **Factory key** (what a fresh bulb ships with) = `5E 36 7B C4`.
- Expose **each light individually** as an HA light entity. **No group APIs.**
- **Persistent boot model:** save our key + the discovered/keyed lights in NVS. On
  boot, restore them (no re-keying) and only key **new** factory-key bulbs.
- **POC mode:** `ENABLE_WIFI=0` in [src/config.h](src/config.h) — no WiFi, no MQTT,
  no HA. Just BLE + a serial console, to prove discovery → key → control over serial.
  Turn on the full bridge later with `ENABLE_WIFI=1`.

---

## 2. STATUS (as of 2026-11-16, evening)

**SOLVED (2026-11-16): `70:e7:f2:72:70:e8` is detected, keyed, and controlled.**
The "no longer in factory state" theory was **wrong** — the bulb was factory all
along. What it actually takes to catch it:
1. **WAKE must be broadcasting when the bulb powers on** — it only emits its
   18-byte factory-identity frame in the seconds after power-up, and only while
   WAKE frames are on the air.
2. **A long discovery window** — `scan 12` (duration is now configurable). The old
   5 s window ended before the bulb finished booting/advertising.
3. **Early power-cycle** — Power OFF ~1 s into the scan, ON ~2 s later, so the
   bulb boots *into the ongoing WAKE* with a full window left to advertise.
4. **Duplicate scan callbacks** (`wantDuplicates=true`) — the bulb interleaves bare
   beacons with identity frames, and the default dedup-by-address scan was dropping
   the identity frames.

**Winning run (serial log, verbatim):**
```
* factory light found: 70:e7:f2:72:70:e8 type=2A A8 (needs keying)
Setting key on light #1, MAC: 70:e7:f2:72:70:e8
new key: 09 90 1D D8
  + keyed light #1 (e770) under our key
```
Control then confirmed physically by the user: `on 1`, `off 1`, `br 32 1`,
`rgb 255 0 0 1`. Our key `09 90 1D D8` + the light list are in NVS.
**Persistence confirmed (2026-11-16):** ESP power-cycled; key + light list restored
from NVS and `on 1` / `off 1` still work. **The POC is done** — detect → key →
control → persist all proven on `70:e7…`.

**Verified working (do not re-litigate these):**
- Build + flash + native-USB serial all work.
- `src/protocol.cpp` is **byte-for-byte identical** to the proven-working reference
  [`dsclee1/BRmesh-esp32-mqtt` `do_generate_command`](https://github.com/dsclee1/BRmesh-esp32-mqtt/blob/7116630d7f4625010da19c3eb179793c45d6b540/src/main.cpp#L267).
  The WAKE we emit is the correct WAKE.
- The active scan works and *sees the bulbs* (they appear in a raw scan).
- Detection + keying logic ([src/main.cpp:251-310](src/main.cpp)) matches the **real**
  captured factory frame (layout in §4).
- **Full detect → key → control on a real bulb** (the run above).

**The 3 bulbs (all share the `f2:72:70:e8` factory OUI):**
| MAC | type code | on Tasmota power switch? | state |
|-----|-----------|--------------------------|-------|
| `70:e7:f2:72:70:e8` | `2A A8` | **YES** | **keyed by us (#1), controlled OK** |
| `2a:e6:f2:72:70:e8` | `2A A8` | NO | untouched |
| `0b:ed:f2:72:70:e8` | `2A A8` | NO | untouched |

**User's standing note:** *"you are forgetting to power-cycle
`70:e7:f2:72:70:e8`. The other two are not on the Tasmota."* The only power we can
drive is `70:e7:f2:72:70:e8`, via the Tasmota switch below.

---

## 3. NEXT STEPS (in order)

**POC (detect → key → control → persist) is COMPLETE for `70:e7…`.**

**Deferred (separate sessions):** other two bulbs (`2a:e6…`, `0b:ed…`) — put on
controllable power or factory-reset via the app, then run the proven recipe below.

> **Proven catch recipe:** `scan 12` → wait ~1 s → Tasmota `Power OFF` → wait ~2 s →
> `Power ON`. The bulb must boot *into* an ongoing WAKE, and the scan window must
> outlast its post-boot advertising. Ready-made driver: `tools/scan_catch.py`.

---

## 4. The FastCon protocol (byte-level)

A command is a **16-byte body** wrapped in an **RF frame**, **whitened**, then
truncated and put in a **BLE manufacturer AD structure**.

### 4.1 16-byte body ([src/protocol.cpp:84-106](src/protocol.cpp))
```
p[0] = (i2 & 0b1111) | ((type & 0b111) << 4) | (forward ? 0x80 : 0)
p[1] = sequence          // rolling counter, starts ~1, ++ each command
p[2] = safe_key          // 0xFF when "no key" (WAKE); else key[3]
p[3] = checksum          // (Σ p[0..15] skipping p[3]) & 0xFF
p[4..15] = data (12 bytes)
```
After the checksum: **header** `p[0..3]` is XORed with the **factory/default key**,
**data** `p[4..15]` is XORed with the **command key** (`key[j & 3]`).

For a **WAKE** specifically: data is 6 zero bytes, key is all-zero, and after the
XOR step the data region `p[4..15]` is **overwritten with the factory key repeated**
(`5E 36 7B C4 5E 36 7B C4 5E 36 7B`).

### 4.2 RF wrap ([src/protocol.cpp:109-131](src/protocol.cpp))
- `size = 0x12 + 3(addr) + 16(data) + 2(CRC)`
- fixed bytes: `b[0x0f]=0x71`, `b[0x10]=0x0f`, `b[0x11]=0x55`
- address `FASTCON_ADDR = {0xC1,0xC2,0xC3}` written **reversed** at offset `0x12`
- body at `0x15..`; a **bit-reversal window** applied over bytes `0x0f..0x11` (and
  neighbours); **CRC16** (init `0xFFFF`, poly `0x1021`, per-byte **reversed** data,
  result `~reverse_16(crc)`) at the last 2 bytes.

### 4.3 Whitening (LFSR, seed `0x25`) then **strip the first 15 bytes** (preamble).
The remaining bytes are the actual air payload → wrapped in the AD structure:
```
[len] 0xFF 0xF0 0xFF <payload...>     // 0xFF=mfg-specific AD, 0xFFF0 company (LE)
```
(`len = payload + 3`.)

### 4.4 Command types (value in `p[0] >> 4`)
- **WAKE = 0** — wake all factory bulbs (data = 6 zeros, all-zero key).
- **SET_KEY = 2** — `data = MAC(6) | number(1) | 0x01 | new_key(4)`; sent **under the
  factory key** (the bulb still has it) so the bulb accepts and stores *our* key.
- **CONTROL = 5** (forward bit set) — the light commands, sent **under our key**.

### 4.5 Control payloads (8-byte `data`, [src/main.cpp:147-178](src/main.cpp))
| cmd | data bytes |
|-----|-----------|
| state ON/OFF | `{0x22, n, on?0x80:0x00, 0,0,0,0,0}` |
| brightness | `{0x22, n, b&127, 0,0,0,0,0}` |
| colour temp | `{0x72, n, 0x80, 0,0,0, t&127, (t>>8)&127}` |
| RGB | `{0x72, n, 0x80, B, R, G, 0}`  (note: blue at d[3], red d[4], green d[5]) |

`n` = the light's number (assigned 1..N when keyed).

### 4.6 The bulb's **factory-identity advertisement** (18-byte mfg, captured from 3 bulbs)
```
F0 FF | 4E | n1 n2 n3 | MAC-reversed(6) | 2A A8 | 5E 36 7B C4
  ↑      ↑      ↑            ↑                ↑         ↑
const  const  rolling    e.g. E8 70 72     type    factory
       type   nonce      F2 E7 70  (= 70:e7:f2:72:70:e8 reversed)
```
- `[2] = 4E` constant; `[3..5]` a rolling nonce (changes frame to frame);
- `[6..11]` = the bulb MAC **reversed**;
- `[12..13] = 2A A8` type; `[14..17] = 5E 36 7B C4` factory key.

**`AddDeviceCallback`** ([src/main.cpp:251-285](src/main.cpp)) accepts a device when:
`mfg.size()==18` AND factory key present at `[14..17]`. It records the 18 bytes
(`mfgData`) for later SET_KEY and adds the light. **`AddLightCallback`**
([src/main.cpp:287-321](src/main.cpp)) marks it `isRegistered` on confirmation.
Both are correct as-is.

**Light type table** ([src/main.cpp:57-62](src/main.cpp)): `39 AE`=Smart (on/off),
`A1 A8`=RGBW, `A0 A8`=RGB, `2A A8`=Color (RGB-capable — our 3 bulbs).

---

## 5. Discovery / keying flow (in code)

- `scan(seconds)` ([src/main.cpp:413](src/main.cpp)) — **concurrent** model (proven by
  the reference): start WAKE advertising → `start` scan → **stop advertising after**.
  interval/window `500`, **active scan**, callback = `AddDeviceCallback`.
- `addLights()` — `scan()`; `delay(1000)`; for each newly-found factory light call
  `addLight(i+1, light)`.
- `addLight()` ([src/main.cpp:455](src/main.cpp)) — builds SET_KEY, **concurrent**
  advertise + 1s scan (interval/window `50`, active), callback = `AddLightCallback`.
  MAC for the SET_KEY payload is read from `mfgData[6..11]`.
- **ESP32 classic-BLE gotcha:** advertise + scan can't both run at full duty, so the
  reference (and we) interleave **start adv → start scan → stop adv**. Do not change to
  a sequential "wake then scan" — that missed the factory frame in testing.

### 5.1 Raw-capture path (how the frames above were observed, not assumed)

The frame layout in §4 and the catch recipe in §2 were **observed, not assumed** —
through a raw-capture debug path built for exactly this. Two pieces made that
possible (this is the same path we'd use to add a new bulb model in the field):

- **The console's `raw` / `rawwake` commands** (see §6) — an
  **unfiltered per-frame hex dump** of every manufacturer advertisement the scan sees
  (duplicates kept); `rawwake` additionally broadcasts our WAKE first. `raw` proves
  what the bulb advertises; `rawwake` proves WAKE triggers the identity frame.
- **Committed Python drivers** in [tools/](tools/) — small, standalone, pyserial-based.
  Serial port via `ESP_SERIAL_PORT` (default `/dev/ttyACM0`; it can swap to `ACM1` on a
  power-cycle); the Tasmota switch for the bulb under test via `TASMOTA_URL`
  (`http://<ip>/cm?cmnd=%s`) — **no LAN IP is stored in the repo**:
  - **`tools/esp_serial.py`** — the low-level plumbing behind every capture:
    `boot [sec]` (DTR/RTS reset + stream the boot log) and `cmd <text> [sec]` (send one
    console line + read the response).
  - **`tools/raw_catch.py`** — the "mirror the phone" capture: reset the chip, drain the
    boot log, send `raw 25` (a **clean scan, no WAKE**), then power-cycle the bulb
    mid-scan via the Tasmota switch and dump the raw manufacturer bytes. This is the
    first look at the 18-byte factory-identity frame (§4.6) exactly as the bulb emits it
    on fresh boot.
  - **`tools/rawwake_catch.py`** — WAKE + raw dump (proved WAKE triggers the identity
    frame).
  - **`tools/scan_catch.py`** — **the proven one**: `scan 12` + early power-cycle that
    actually keyed `70:e7…`.

---

## 6. Serial console (POC, ENABLE_WIFI=0)

Type over 115200 baud. `help` for the full list:
```
list                 known lights
sel <n>              set target light number
on [n]  off [n]      state
br <0-127> [n]       brightness
ct <0-32767> [n]     colour temperature
rgb r g b [n]        colour
scan [sec=5]         re-run discovery / key new bulbs (use 12+ with a mid-scan power-cycle)
bootcatch [sec=12]   WAKE+concurrent scan (run while power-cycling a bulb)
raw [sec=5]          unfiltered BLE dump (debug, duplicates)
rawwake [sec=5]      WAKE broadcast + every frame (debug — proved WAKE triggers the identity frame)
wake [sec=3]         broadcast our WAKE only
wakephone [sec=3]    replay the phone's 24-byte WAKE (debug, not the primary path)
```

---

## 7. Build / flash / serial toolchain

- **PlatformIO binary** (the *only* one that works): `/home/rao/.platformio/penv/bin/pio`
- **Python** (for serial helpers): `/home/rao/.platformio/penv/bin/python`
- Platform = **pioarduino / platform-espressif32** (NOT the stock espressif), see
  [platformio.ini](platformio.ini). Board `waveshare_esp32_s3_zero`, 4 MB flash, 2 MB PSRAM.
- **Native USB serial** (no UART bridge): `-DARDUINO_USB_MODE=1 -DARDUINO_USB_CDC_ON_BOOT=1`.
  Serial port = **`/dev/ttyACM0` or `/dev/ttyACM1` (115200) — it can swap between
  the two on power-cycles!** Check with `ls /dev/ttyACM*`; override with
  `ESP_SERIAL_PORT=/dev/ttyACM0 ...`.

```bash
P=/home/rao/.platformio/penv/bin
# build + flash
$P/pio run -e esp32s3_devkit --target upload
# read serial (tools/esp_serial.py resets the chip via DTR/RTS and streams output)
$P/python tools/esp_serial.py boot 12      # reset + capture boot log
$P/python tools/esp_serial.py cmd "on 1" 4# send one console line
```

**Power toggle for `70:e7:f2:72:70:e8`** (a Tasmota smart switch — set its IP per
machine, not stored in the repo):
```bash
TASMOTA="http://<tasmota-ip>/cm"
curl -s "$TASMOTA?cmnd=Power%20OFF"
curl -s "$TASMOTA?cmnd=Power%20ON"
curl -s "$TASMOTA?cmnd=Power"          # read state
```
Only **one** request per state change — do **not** loop/retry (the sandbox classifier
flags repeated network calls as DoS; it is flaky and has denied `pio run` / inline
python before). If a command is denied, **stop and report**, don't retry verbatim.
The other two bulbs are **not** on this switch.

Ready-made test drivers (see [tools/](tools/), all env-driven, no LAN IP in VCS):
**`tools/scan_catch.py`** (the proven one — `scan 12` + early power-cycle),
`tools/rawwake_catch.py` (WAKE + raw dump), `tools/raw_catch.py` (plain scan).
Set `ESP_SERIAL_PORT` and `TASMOTA_URL` per machine; edit timings there before reuse.

---

## 8. References

Two independent community reverse-engineerings of the same BroadLink **FastCon** air
protocol preceded and informed ours. **Credit where due** — our `protocol.cpp` frame
packing is derived from the first; the second is a useful cross-check and documents
the `controller_id`/`mesh_key` model we implement.

- **`dsclee1/BRmesh-esp32-mqtt`** —
  <https://github.com/dsclee1/BRmesh-esp32-mqtt> (ESP32 + MQTT, 2024).
  **The ground truth our `protocol.cpp` mirrors byte-for-byte.**
  `src/main.cpp` (at commit `7116630`):
  - `do_generate_command` — [main.cpp#L267](https://github.com/dsclee1/BRmesh-esp32-mqtt/blob/7116630d7f4625010da19c3eb179793c45d6b540/src/main.cpp#L267)
  - `get_rf_payload` — `#L236`; `whiteningEncode` — `#L160`;
    `package_ble_fastcon_body` — `#L90`
  - `DEFAULT_BLE_FASTCON_ADDRESS` `{0xC1,0xC2,0xC3}` — `#L71`
  - **factory/default key `5E 36 7B C4`** — `#L32`
  - `scan()` — `#L544` (**concurrent** WAKE+scan); `addLight()` — `#L567`
    (concurrent, active scan); `addLights()` — `#L600`.
- **`scross01/esphome-fastcon`** —
  <https://github.com/scross01/esphome-fastcon> (esphome component, active 2026).
  Independent, cleaner implementation with a dedicated
  [`components/fastcon/protocol.{h,cpp}`](https://github.com/scross01/esphome-fastcon/tree/main/components/fastcon)
  module + `fastcon_controller` (owns `mesh_key_`) + `fastcon_light`. Good cross-check
  on the frame layout and the keying model. **Note the divergence:** its README's
  documented path is to **extract the key from the phone app via `adb logcat`** —
  the exact approach this project deliberately avoids (see the key-originator model
  in §1 and the standing rule below). We generate our own key and write it onto the
  bulbs instead of pulling one from the app.
- **Our firmware:** [src/main.cpp](src/main.cpp) (device/discovery/console/HA),
  [src/protocol.cpp](src/protocol.cpp) (FastCon), [src/config.h](src/config.h)
  (WiFi/MQTT/LED/SCAN_DURATION, `ENABLE_WIFI`).
- **Captured factory frames** (3 bulbs) and the phone's 24-byte WAKE are in the prior
  session transcripts under
  `~/.claude/projects/-mnt-gfdata-platformIO-br-mesh-controller/` if byte-level
  detail is needed.

---

## 9. Gotchas / standing rules

- **DO NOT** extract the key from the Android app (key-originator model is the point).
- **DO NOT** loop/retry network calls or pin-toggling — classifier flags them; stop and
  report on a denial. User handles any `sudo`/`chown` themselves.
- The two `#if ENABLE_WIFI` blocks (HA entity, HTTP, MQTT) are **dead code in the POC**
  (`ENABLE_WIFI=0`) but must keep compiling when flipped to 1 — the HA lib
  (`dawidchyrzynski/home-assistant-integration`) is commented out of
  [platformio.ini](platformio.ini) and re-added when we go live.
- `monitor_port=/dev/ttyACM0` in [platformio.ini](platformio.ini) is stale but harmless;
  `/dev/ttyACM1` is the live one.
