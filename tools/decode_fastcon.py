#!/usr/bin/env python3
# decode_fastcon.py — invert protocol.cpp's FastCon encode pipeline (full decoder).
#
# Air frame (captured after the "F0 FF" company prefix) = LFSR-whitened RF frame
# with the first 15 preamble bytes already stripped. Inverse: prepend the 15
# preamble bytes, de-whiten (LFSR seed 0x25), read the 16-byte body at RF offset
# 0x15 (== byte 6 of the 24-byte payload, after the 0x71 0x0F 0x55 C1 C2 C3 prefix).
#
#   body[0:4]  = header, XORed with the FACTORY key (5E 36 7B C4)  -- NEVER the mesh key
#   body[4:16] = 12-byte data, XORed with the 4-byte MESH key (repeating)
#
# The header needs NO key -> type/seq/safekey/checksum are always readable.
# The data region needs the mesh key. We either take it via --key, or auto-recover
# it from a batch: on an 8-byte command body[12:16] is zero-padding XOR key = the
# raw 4-byte mesh key, so its mode across frames is the key. The command length is
# found by the build_body checksum (protocol.cpp:100):
#   body_cksum == (p[0]+p[1]+p[2] + sum(plaintext_data[:len])) & 0xFF
#
# Confirmed on capture_20261118_174310.log: header XORs with FACTORY even on
# CONTROL (checksum closes 267 frames under FACTORY, 0 under the mesh key); the
# recovered mesh key is 36 38 37 39; command opcodes seen: 0x21 assign,
# 0x22 single on/off, 0x43/0x93 group (2A A8 prefix), 0x72 single color.
#
# usage:
#   decode_fastcon.py 6d b6 43 ...             # one captured payload (after F0 FF)
#   decode_fastcon.py --key 36383739 6d ...    # one payload + explicit mesh key
#   decode_fastcon.py --file cap.txt           # batch: auto-recover key, distinct payloads
#   decode_fastcon.py --relay cap.txt          # per-frame table + fwd/seq summaries +
#                                              # relay-pair hunt (same type/i2/seq seen
#                                              # with a flipped forward bit or 2 checksums)
import sys
import re
from collections import Counter, defaultdict

FACTORY = [0x5e, 0x36, 0x7b, 0xc4]  # protocol::DEFAULT_KEY
SEED = 0x25
PREAMBLE = 15                        # bytes stripped by generate_command
NAMES = {0: "WAKE", 2: "SET_KEY", 5: "CONTROL"}


def de_whiten(payload, preamble=PREAMBLE, seed=SEED):
    full = [0] * preamble + list(payload)
    # ctx init (whitening_init)
    ctx = [1, (seed >> 5) & 1, (seed >> 4) & 1, (seed >> 3) & 1,
           (seed >> 2) & 1, (seed >> 1) & 1, seed & 1]
    out = []
    for b in full:
        ctx0, ctx1, ctx2, ctx3, ctx4, ctx5, ctx6 = ctx
        ctx52 = ctx5 ^ ctx2
        ctx41 = ctx4 ^ ctx1
        ctx63 = ctx6 ^ ctx3
        ctx630 = ctx63 ^ ctx0
        c = ((b & 0x80) ^ ((ctx52 ^ ctx6) << 7)) \
          + ((b & 0x40) ^ (ctx630 << 6)) \
          + ((b & 0x20) ^ (ctx41 << 5)) \
          + ((b & 0x10) ^ (ctx52 << 4)) \
          + ((b & 0x08) ^ (ctx63 << 3)) \
          + ((b & 0x04) ^ (ctx4 << 2)) \
          + ((b & 0x02) ^ (ctx5 << 1)) \
          + ((b & 0x01) ^ (ctx6 << 0))
        out.append(c & 0xff)
        # ctx update (same order as whitening_encode)
        ctx = [ctx52 ^ ctx6, ctx630, ctx41, ctx52,
               ctx52 ^ ctx3, ctx630 ^ ctx4, ctx41 ^ ctx5]
    return out


def body_at(rf, off=0x15):
    return rf[off:off + 16]


def full_body(payload):
    """16-byte body, or None if the frame is too short to hold one (truncated capture)."""
    rf = de_whiten(payload, PREAMBLE)
    if len(rf) < 0x15 + 16:
        return None
    return body_at(rf)


def hexs(bs):
    return " ".join("%02x" % b for b in bs)


def header(body):
    # header XORed with factory key (never the mesh key -- see module docstring)
    p = [body[i] ^ FACTORY[i & 3] for i in range(4)]
    b0 = p[0]
    typ = (b0 >> 4) & 0x7
    return {
        "type": typ, "type_name": NAMES.get(typ, "?"),
        "forward": bool(b0 & 0x80), "i2": b0 & 0x0f,
        "seq": p[1], "safe_key": p[2], "checksum": p[3],
    }


def plaintext_data(body, key):
    return [body[4 + j] ^ key[j & 3] for j in range(12)]


def cmd_len(pt_head, data):
    """Command length that closes the build_body checksum, else None."""
    for ln in range(1, 13):
        if (pt_head[0] + pt_head[1] + pt_head[2] + sum(data[:ln])) & 0xFF == pt_head[3]:
            return ln
    return None


def classify(typ, ln, data):
    if typ == 0:
        return "WAKE (boot / keying trigger)"
    if typ == 2:
        return "SET_KEY (write mesh key onto a factory bulb)"
    if typ != 5:
        return "control (unknown type)"
    op = data[0]
    if op == 0x21:
        return "ASSIGN  light=%02x -> group=%02x" % (data[1], data[2] if ln > 2 else 0)
    if (op & 0xF) == 0x03:                       # group control; 2A A8 prefix + group_id
        gid = data[3] if ln > 3 else None
        extra = ("  payload=" + hexs(data[4:])) if ln > 4 else ""
        return "GROUP %02x  group_id=%s%s" % (op, ("%02x" % gid) if gid is not None else "(all)", extra)
    if (op & 0xF) == 0x0C:
        return "EFFECTS len=%d" % ln
    if op == 0x72:
        return "SINGLE color light=%02x" % (data[1] if ln > 1 else 0)
    if op == 0x22:
        state = "ON" if (ln > 2 and data[2] & 0x80) else "OFF"
        return "SINGLE on/off light=%02x %s" % (data[1] if ln > 1 else 0, state)
    return "control opcode=0x%02x len=%d" % (op, ln)


def recover_key(bodies):
    """Mode of body[12:16]: on an 8-byte command those bytes are 0-pad XOR key = raw key."""
    votes = Counter(tuple(b[12:16]) for b in bodies)
    if not votes:
        return None
    return list(votes.most_common(1)[0][0])


def decode(payload, key=None, preamble=PREAMBLE, quiet=False):
    rf = de_whiten(payload, preamble)
    if len(rf) < 0x15 + 16:
        return "  (frame too short: %d bytes after de-whiten; expected >= %d)" % (len(rf), 0x15 + 16)
    body = body_at(rf)
    h = header(body)
    if not quiet:
        print("  body@0x15 (%d b): %s" % (len(body), hexs(body)))
        print("  header: type=%d %s  fwd=%d i2=%d seq=0x%02x safekey=0x%02x cksum=0x%02x"
              % (h["type"], h["type_name"], h["forward"], h["i2"],
                 h["seq"], h["safe_key"], h["checksum"]))
    if key is None:
        if not quiet:
            print("  data (raw, mesh key unknown -- pass --key or use --file to auto-recover): %s" % hexs(body[4:]))
        return h
    data = plaintext_data(body, key)
    pt_head = [body[i] ^ FACTORY[i & 3] for i in range(4)]
    ln = cmd_len(pt_head, data)
    if not quiet:
        if ln is None:
            # WAKE/SET_KEY use the factory key for the data region, not the mesh key
            print("  data (len not checksum-valid with mesh key -> WAKE/SET_KEY; raw): %s" % hexs(data))
        else:
            print("  data (len=%d): %s" % (ln, hexs(data[:ln])))
            print("  command: %s" % classify(h["type"], ln, data[:ln]))
    return h


def parse_log(path):
    """Return [(ts, mac, payload)] for every FastCon (F0 FF) frame in a capture log.

    ts/mac come from the staged_capture line shape
    `[HH:MM:SS.mmm] t=NNNs RAW <mac> mfg(N): <hex>`; each is "" when a line
    lacks that prefix, so this still works on a bare `mfg(N): <hex>` dump."""
    ts_rx = re.compile(r"\[([^\]]*)\]")
    mac_rx = re.compile(r"RAW ([0-9a-f:]{17})")
    out = []
    for line in open(path).read().splitlines():
        if "mfg(" not in line:
            continue
        b = [int(x, 16) for x in line.split("mfg(", 1)[1].split("):", 1)[1].split()]
        if len(b) < 2 or b[0] != 0xF0 or b[1] != 0xFF:
            continue
        ts = ts_rx.search(line)
        mac = mac_rx.search(line)
        out.append((ts.group(1) if ts else "", mac.group(1) if mac else "", b[2:]))
    return out


def decode_frame(payload, key=None):
    """One payload -> (body, header, data, ln), or None if the frame is truncated."""
    body = full_body(payload)
    if body is None:
        return None
    h = header(list(body))
    data = plaintext_data(list(body), key) if key else None
    pt_head = [body[i] ^ FACTORY[i & 3] for i in range(4)]
    ln = cmd_len(pt_head, data) if data is not None else None
    return body, h, data, ln


def relay_analysis(path, key=None):
    """Per-frame table + forward/seq summaries + the relay-pair hunt.

    The hunt is the point of the mesh-relay question: the SAME (type, i2, seq)
    seen with >1 forward bit, or the same command body seen with 2 different
    checksums, is direct evidence that bulbs re-broadcast (relay) a command
    rather than each originating its own — the header seq stays put while the
    checksum/forward flip per re-broadcaster (protocol.md §2.8)."""
    frames = parse_log(path)
    print("FastCon (F0 FF) frames: %d  |  distinct MACs: %d"
          % (len(frames), len(set(m for _, m, _ in frames if m))))
    bodies = [full_body(p) for _, _, p in frames]
    bodies = [b for b in bodies if b is not None]
    if key is None:
        key = recover_key(bodies) if bodies else None
    print("recovered mesh key: %s" % (hexs(key) if key else "(none)"))

    # per-frame table (condensed): time, mac, type, fwd, i2, seq, cksum, command
    print("\n%-13s %-18s %-6s %-3s %-3s %-5s %-6s  %s"
          % ("time", "mac", "type", "fwd", "i2", "seq", "cksum", "command"))
    decoded = []
    for ts, mac, p in frames:
        d = decode_frame(p, key)
        if d is None:
            continue
        _, h, data, ln = d
        cmd = classify(h["type"], ln, data[:ln]) if (ln is not None and key) else ""
        decoded.append((ts, mac, h))
        print("%-13s %-18s %-6s %-3d %-3d %-5s %-6s  %s"
              % (ts, mac, h["type_name"], h["forward"], h["i2"],
                 "0x%02x" % h["seq"], "0x%02x" % h["checksum"], cmd))

    # summaries
    print("\nBy (type, fwd):")
    g = Counter((h["type_name"], h["forward"]) for _, _, h in decoded)
    for (ty, fwd), v in sorted(g.items()):
        print("  %-8s fwd=%d : %d" % (ty, fwd, v))

    print("\nBy (type, i2, fwd):")
    g2 = Counter((h["type_name"], h["i2"], h["forward"]) for _, _, h in decoded)
    for (ty, i2, fwd), v in sorted(g2.items()):
        print("  %-8s i2=%d fwd=%d : %d" % (ty, i2, fwd, v))

    # relay-pair hunt: same (type, i2, seq) with >1 distinct forward bit or checksum
    print("\nRelay-pair hunt — (type,i2,seq) with >1 fwd or >1 checksum:")
    byseq = defaultdict(lambda: {"fwd": set(), "cksum": set(), "mac": set()})
    for ts, mac, h in decoded:
        e = byseq[(h["type_name"], h["i2"], h["seq"])]
        e["fwd"].add(h["forward"])
        e["cksum"].add(h["checksum"])
        e["mac"].add(mac)
    found = 0
    for k, e in sorted(byseq.items()):
        if len(e["fwd"]) > 1 or len(e["cksum"]) > 1:
            found += 1
            print("  %-8s i2=%d seq=0x%02x  fwd=%s  cksum=%s  macs=%d"
                  % (k[0], k[1], k[2], sorted(e["fwd"]),
                     ["0x%02x" % c for c in sorted(e["cksum"])], len(e["mac"])))
    if not found:
        print("  (none — no same-seq fwd flip or checksum mutation seen)")


def main():
    args = sys.argv[1:]
    key = None
    if args and args[0] == "--key":
        key = [int(args[1][i:i + 2], 16) for i in range(0, 8, 2)]
        args = args[2:]
    if args and args[0] == "--relay":
        relay_analysis(args[1], key=key)
        return
    if args and args[0] == "--file":
        frames = [p for _, _, p in parse_log(args[1])]
        if not frames:
            print("  (no F0 FF frames in %s)" % args[1])
            return
        # keep only full frames; auto-recover the mesh key from padding; decode distinct bodies
        bodies = [full_body(p) for p in frames]
        bodies = [b for b in bodies if b is not None]
        short = len(frames) - len(bodies)
        rec = recover_key(bodies)
        print("  %d F0 FF frames (%d full, %d too-short ignored)" % (len(frames), len(bodies), short))
        print("  recovered mesh key: %s%s"
              % (hexs(rec) if rec else "(none)", " (from body[12:16] padding mode)"))
        if key is None:
            key = rec
        by_body = Counter(tuple(b) for b in bodies)
        print("\n  distinct payloads (%d):" % len(by_body))
        for body, n in by_body.most_common():
            h = header(list(body))
            data = plaintext_data(list(body), key) if key else None
            ln = cmd_len([body[i] ^ FACTORY[i & 3] for i in range(4)], data) if data else None
            if ln is not None and key:
                desc = "%s (len=%d)" % (classify(h["type"], ln, data[:ln]), ln)
                shown = hexs(data[:ln])
            else:
                desc = h["type_name"]
                shown = hexs(list(body[4:]))
            print("    x%-4d type=%-8s data=%s   ->  %s" % (n, h["type_name"], shown, desc))
        return
    if not args:
        print("usage: decode_fastcon.py <hexbytes...> [--key aabbccdd] | --file <log> | --relay <log>")
        return
    payload = [int(x, 16) for x in args]
    print("=== payload (%d b) ===" % len(payload))
    decode(payload, key=key)


if __name__ == "__main__":
    main()
