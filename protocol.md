# protocol.md — the FastCon air protocol (byte-level)

**FastCon** is BroadLink's **patented** BLE advertising/broadcast interface used
by the BRGD / BRMesh bulbs. It is **not real BLE Mesh** — no pairing, no
connection, no acknowledgements. Commands travel as BLE advertising/broadcast
frames the bulbs simply listen to. This file is the byte-level reference for that
protocol: how a command is framed, the command types, the control payloads, the
bulbs' identity advertisement, and where the layout came from.

Implementation: [src/protocol.cpp](src/protocol.cpp) (frame packing —
byte-identical to the community reference), [src/MeshController.cpp](src/MeshController.cpp)
(control payloads, discovery/keying, NVS persistence) and
[src/console.cpp](src/console.cpp) (the serial POC UI). Project model, status, and
build/flash/test live in [CLAUDE.md](CLAUDE.md).

---

## 1. How the protocol was obtained

The framing below was reverse-engineered from **manual over-the-air captures**
(broadcast frames — nothing here requires a connection), not from any SDK or
vendor documentation:

1. **Bulb factory-identity frames** — the 18-byte manufacturer advertisements
   captured from all three bulbs (`70:e7…`, `2a:e6…`, `0b:ed…`) in factory state.
   Layout in §2.6. These exposed the MAC-reversed identity, the type code, and —
   the key enabler — the **factory key `5E 36 7B C4` sitting in the clear** in the
   advertisement, which is what makes SET_KEY possible.
2. **The phone app's WAKE broadcast** — the 24-byte WAKE payload the BRGD app
   emits, captured live during use; replayed by the `wakephone` console command
   (debug path, not the primary one).
3. **Keyed identity frames** — after SET_KEY the bulb re-advertises the same
   18-byte shape but with **our** key at `[14..17]`; `AddLightCallback` decrypts
   the assigned number with our key. This confirmation loop is what made keying
   observable and verifiable.

Two independent community reverse-engineerings of the same FastCon air protocol
preceded and informed ours — see §3 (References).

---

## 2. A command = 16-byte body → RF frame → whitened → BLE AD structure

### 2.1 16-byte body ([src/protocol.cpp:84-106](src/protocol.cpp))
```
p[0] = (i2 & 0b1111) | ((type & 0b111) << 4) | (forward ? 0x80 : 0)
p[1] = sequence          // rolling counter, starts ~1, ++ each command
p[2] = safe_key          // 0xFF when "no key" (WAKE); else key[3]
p[3] = checksum          // (Σ p[0..15] skipping p[3]) & 0xFF
p[4..15] = data (12 bytes)
```
After the checksum: **header** `p[0..3]` is XORed with the **factory/default key**,
**data** `p[4..15]` is XORed with the **command key** (`key[j & 3]`).

**The header is *always* factory-XORed — even on CONTROL.** It is not a WAKE/SET_KEY
special case. A/B proof over `capture_20261118_174310.log` (326 full frames): with the
header XORed by the factory key, the checksum above closes on **267** CONTROL frames and
`type` reads `5`; with the header XORed by the mesh key, it closes on **0**. So the only
region that ever carries the mesh key is the data region `p[4..15]`.

For a **WAKE** specifically: data is 6 zero bytes, key is all-zero, and after the
XOR step the data region `p[4..15]` is **overwritten with the factory key repeated**
(`5E 36 7B C4 5E 36 7B C4 5E 36 7B`).

### 2.2 RF wrap ([src/protocol.cpp:109-131](src/protocol.cpp))
- `size = 0x12 + 3(addr) + 16(data) + 2(CRC)`
- fixed bytes: `b[0x0f]=0x71`, `b[0x10]=0x0f`, `b[0x11]=0x55`
- address `FASTCON_ADDR = {0xC1,0xC2,0xC3}` written **reversed** at offset `0x12`
- body at `0x15..`; a **bit-reversal window** applied over bytes `0x0f..0x11` (and
  neighbours); **CRC16** (init `0xFFFF`, poly `0x1021`, per-byte **reversed** data,
  result `~reverse_16(crc)`) at the last 2 bytes.

### 2.3 Whitening (LFSR, seed `0x25`) then **strip the first 15 bytes** (preamble).
The remaining bytes are the actual air payload → wrapped in the AD structure:
```
[len] 0xFF 0xF0 0xFF <payload...>     // 0xFF=mfg-specific AD, 0xFFF0 company (LE)
```
(`len = payload + 3`.)

### 2.4 Command types (value in `p[0] >> 4`)
- **WAKE = 0** — wake all factory bulbs (data = 6 zeros, all-zero key).
- **SET_KEY = 2** — `data = MAC(6) | number(1) | 0x01 | new_key(4)`; sent **under the
  factory key** (the bulb still has it) so the bulb accepts and stores *our* key.
- **CONTROL = 5** (forward bit set) — the light commands, sent **under our key**.

### 2.5 Control payloads (8-byte `data`, [src/MeshController.cpp:82-113](src/MeshController.cpp))
| cmd | data bytes |
|-----|-----------|
| state ON/OFF | `{0x22, n, on?0x80:0x00, 0,0,0,0,0}` |
| brightness | `{0x22, n, b&127, 0,0,0,0,0}` |
| colour temp | `{0x72, n, 0x80, 0,0,0, t&127, (t>>8)&127}` |
| RGB | `{0x72, n, 0x80, B, R, G, 0}`  (note: blue at d[3], red d[4], green d[5]) |

`n` = the light's number (assigned 1..N when keyed).

### 2.6 The bulb's **factory-identity advertisement** (18-byte mfg, captured from 3 bulbs)
```
F0 FF | 4E | n1 n2 n3 | MAC-reversed(6) | 2A A8 | 5E 36 7B C4
  ↑      ↑      ↑            ↑                ↑         ↑
const  const  rolling    e.g. E8 70 72     type    factory
       type   nonce      F2 E7 70  (= 70:e7:f2:72:70:e8 reversed)
```
- `[2] = 4E` constant; `[3..5]` a rolling nonce (changes frame to frame);
- `[6..11]` = the bulb MAC **reversed**;
- `[12..13] = 2A A8` type; `[14..17] = 5E 36 7B C4` factory key.

**`AddDeviceCallback`** ([src/MeshController.cpp:167](src/MeshController.cpp)) accepts a device when:
`mfg.size()==18` AND factory key present at `[14..17]`. It records the 18 bytes
(`mfgData`) for later SET_KEY and adds the light. **`AddLightCallback`**
([src/MeshController.cpp:195](src/MeshController.cpp)) marks it `isRegistered` on confirmation.
Both are correct as-is.

**Light type table** ([src/MeshController.cpp:12](src/MeshController.cpp)): `39 AE`=Smart (on/off),
`A1 A8`=RGBW, `A0 A8`=RGB, `2A A8`=Color (RGB-capable — our 3 bulbs).

### 2.7 Group & assign commands (app-side; recovered from the phone's captures)

The BRGD **app** groups bulbs and drives them by group — under the app's own mesh key.
**Reference only: this project does NOT adopt it.** We are the key originator
([CLAUDE.md](CLAUDE.md) §1) and expose *each light individually* (no group APIs).
Documented here because a bulb the app has grouped may only answer group frames, and
[tools/decode_fastcon.py](tools/decode_fastcon.py) has to read them.

The app's frames were decoded with the **app's mesh key `36 38 37 39`** — *recovered*,
not guessed, by two independent methods:
1. on an 8-byte command, `body[12:16]` is zero-padding XOR key = the raw key
   (mode across frames → `36 38 37 39`);
2. checksum-voting: that key makes the §2.1 checksum close on the most frames.

That is the key the **app** wrote onto the bulbs. Ours is generated by `MeshController`
and written via SET_KEY — a different value.

Opcodes seen in the capture (all checksum-valid; header factory-XORed per §2.1):

| opcode | meaning | example data |
|--------|---------|--------------|
| `0x21` | **ASSIGN** light → group | `21 01 01` = light #1 → group 1 |
| `0x22` | **SINGLE** on/off (d[2]&0x80 = ON) | `22 01 80` ON · `22 01` OFF |
| `0x43` | **GROUP** on/off | `43 2A A8 <gid>` |
| `0x93` | **GROUP** colour | `93 2A A8 <gid> …` |
| `0x72` | **SINGLE** colour | `72 <light> 80 B R G …` |

- Group frames are keyed by low nibble `0x03` and address the group as
  `<2A A8 type-code> <group_id>` (i.e. `data[1..3]`), then carry the payload.
- `0x22` (single on/off/brightness) and `0x72` (single colour) are the **same**
  opcodes we send in §2.5 — the app just runs them against its own key and light numbers.
- WAKE / SET_KEY data regions ride the **factory** key, not the mesh key (§2.4), so
  `decode_fastcon.py` handles them separately.

---

## 3. References

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
  the exact approach this project deliberately avoids (see the key-originator model in
  CLAUDE.md §1). We generate our own key and write it onto the bulbs instead of
  pulling one from the app.
