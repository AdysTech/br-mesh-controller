#!/usr/bin/env python3
# staged_capture.py — interactive, ENTER-driven BLE capture driver.
#
# A stage is UNBOUNDED: we start a long passive capture and listen LIVE until
# YOU press ENTER, then we send 'scanstop' to the ESP to end it. The scan
# duration (CAP_SEC) is only a safety cap (insurance if you walk away) — ENTER
# is what actually ends a stage. That is the whole point of 'scanstop' existing.
#
# Why passive (rawp): we're hunting the PHONE's one-shot FastCon frames — the
# app's group-control (type 3) and assign-to-group (0x21) broadcasts. Passive
# listening transmits no SCAN_REQ, so it doesn't steal RX time from a one-shot.
# (Use CAP_CMD=raw for an active scan instead, if you specifically want it.)
#
# Flow per stage:
#   1. you type a stage name          (q / Enter quits the whole session)
#   2. we send '<CAP_CMD> <CAP_SEC>'  and confirm the ESP is listening
#   3. LIVE: every frame is printed + logged the instant it arrives
#   4. you do your thing in the app, then press ENTER when done
#   5. we send 'scanstop', drain the tail, print a per-stage summary
#   6. back to (1) — as many stages as you like
#
# Env:
#   ESP_SERIAL_PORT  serial port (default /dev/ttyACM0)
#   CAP_CMD          capture command (default 'rawp'; set 'raw' for active)
#   CAP_SEC          safety cap on scan duration (default 3600); ENTER ends early
# Requires: pyserial.
import os, re, sys, time, threading
import serial

PORT     = os.environ.get("ESP_SERIAL_PORT", "/dev/ttyACM0")
BAUD     = 115200
CAP_CMD  = os.environ.get("CAP_CMD", "rawp")
CAP_SEC  = int(os.environ.get("CAP_SEC", "3600"))

LOGPATH = os.path.join(os.path.dirname(__file__),
                       "capture_%s.log" % time.strftime("%Y%m%d_%H%M%S"))

# our three bulb MACs (lowercase, as the ESP prints them) — tagged [OURS] in summaries
OURS = ("70:e7", "2a:e6", "0b:ed")

MAC_RE  = re.compile(r"RAW\s+([0-9a-f:]+)", re.I)
MFG_RE  = re.compile(r"mfg\((\d+)\):\s*(.*)$")


def ts():
    return time.strftime("%H:%M:%S") + "." + ("%03d" % int((time.time() * 1000) % 1000))


def hr(ch="=", n=76):
    return ch * n


class Reader(threading.Thread):
    """Reads serial lines for the whole session; prints each live + timestamped,
    logs it, and appends to the current stage buffer (reader.bufref) when set.
    The stream is long-lived, so a quiet period (read-timeout -> b'') just loops
    rather than terminating the thread."""
    def __init__(self, s, logf):
        super().__init__(daemon=True)
        self.s = s
        self.logf = logf
        self.bufref = None                 # list; the main thread re-points this per stage
        self.start_seen = threading.Event()
    def _emit(self, t):
        line = "[%s] %s" % (ts(), t)
        print("    " + line); sys.stdout.flush()
        self.logf.write(line + "\n"); self.logf.flush()
        low = t.lower()
        if "scan" in low and ("unfiltered" in low or "listening" in low
                              or "every advertiser" in low or "no scan_req" in low):
            self.start_seen.set()
        if self.bufref is not None:
            self.bufref.append(t)
    def run(self):
        while True:
            line = self.s.readline()
            if not line:                    # read timeout / quiet — keep listening
                continue
            t = line.decode(errors="replace").rstrip("\n")
            if t:
                self._emit(t)


def wait_start(reader, timeout=6):
    """Wait until the ESP confirms the scan is actually running."""
    deadline = time.time() + timeout
    while not reader.start_seen.is_set() and time.time() < deadline:
        time.sleep(0.1)
    if reader.start_seen.is_set():
        print("  [ok] ESP is listening -> GO AHEAD. (press ENTER to end the stage)\n")
    else:
        print("  [warn] no start banner in %ss; proceeding anyway.\n" % timeout)


def _parse(l):
    """Return (mac, mfg_len, payload) for a 'RAW <mac> mfg(N): <bytes>' line, else None."""
    mm = MFG_RE.search(l)
    if not mm:
        return None
    macm = MAC_RE.search(l)
    mac = macm.group(1).lower() if macm else "?"
    return mac, int(mm.group(1)), mm.group(2).strip()


def summarize(name, buf):
    """Digest a stage: distinct payloads with length, count, source MAC, ours-tag.
    The source-MAC column is what separates the PHONE's group frames from ours."""
    frames = [l for l in buf if "mfg(" in l]
    ours_n = sum(1 for l in frames if any(o in l.lower() for o in OURS))
    print("\n  --- %s summary ---" % name)
    print("  mfg frames: %d   (ours: %d)" % (len(frames), ours_n))
    by_payload = {}
    for l in frames:
        p = _parse(l)
        if not p:
            continue
        mac, ln, payload = p
        e = by_payload.setdefault((ln, payload), {"n": 0, "macs": set()})
        e["n"] += 1
        e["macs"].add(mac)
    if by_payload:
        print("  distinct payloads   (x=count  len  source-MACs):")
        for (ln, payload), e in sorted(by_payload.items(), key=lambda kv: -kv[1]["n"]):
            is_ours = any(o in m for m in e["macs"] for o in OURS)
            tag = "OURS" if is_ours else "     "
            print("    x%-3d mfg(%-2d) [%s] %s   <- %s"
                  % (e["n"], ln, tag, payload, ",".join(sorted(e["macs"]))))
    else:
        print("    (no mfg frames captured)")
    print(hr())


def main():
    print(hr())
    print("  staged_capture   port=%s  cmd=%s  (safety cap %ds; ENTER ends a stage)"
          % (PORT, CAP_CMD, CAP_SEC))
    print("  type a stage name to capture it, then press ENTER to stop it")
    print("  q / Enter at the stage-name prompt quits the session")
    print("  log : %s" % LOGPATH)
    print(hr())

    s = serial.Serial(PORT, BAUD, timeout=0.4)
    time.sleep(0.05)
    s.reset_input_buffer()

    logf = open(LOGPATH, "w")
    reader = Reader(s, logf)
    reader.start()

    def logsep(title):
        logf.write("\n" + hr() + "\n" + title + "  (%s)\n" % ts() + hr() + "\n")
        logf.flush()

    allbuf = []
    seq = 0
    try:
        while True:
            try:
                name = input("\n  >> stage name (q / Enter to quit): ").strip()
            except (EOFError, KeyboardInterrupt):
                name = ""
            if not name or name.lower() in ("q", "quit", "exit"):
                break
            seq += 1
            title = "STAGE %d — %s" % (seq, name)

            print("\n" + hr())
            print("  " + title)
            print("  capture: %s  —  DO YOUR ACTION IN THE APP, then press ENTER to stop" % CAP_CMD)
            print(hr())
            logsep(title)

            buf = []
            reader.bufref = buf
            reader.start_seen.clear()
            s.write(("%s %d\r\n" % (CAP_CMD, CAP_SEC)).encode())
            print("  >> sent: %s %d" % (CAP_CMD, CAP_SEC))
            wait_start(reader)

            try:
                input("  >> (capturing… press ENTER when you've done the action) ")
            except (EOFError, KeyboardInterrupt):
                pass

            # end the scan, drain the tail, then digest
            reader.bufref = None
            s.write(b"scanstop\r\n")
            print("  >> sent: scanstop")
            time.sleep(0.6)
            summarize(title, buf)
            allbuf.extend(buf)
    except KeyboardInterrupt:
        print("\n  >> interrupted — stopping.")
        try:
            s.write(b"scanstop\r\n")
        except Exception:
            pass
    finally:
        print("\n" + hr())
        print("  CONSOLIDATED — distinct payloads across all %d stage(s)" % seq)
        print(hr())
        by_payload = {}
        for l in allbuf:
            p = _parse(l)
            if not p:
                continue
            mac, ln, payload = p
            e = by_payload.setdefault((ln, payload), {"n": 0, "macs": set()})
            e["n"] += 1
            e["macs"].add(mac)
        if by_payload:
            for (ln, payload), e in sorted(by_payload.items(), key=lambda kv: -kv[1]["n"]):
                is_ours = any(o in m for m in e["macs"] for o in OURS)
                tag = "OURS" if is_ours else "     "
                print("  x%-4d mfg(%-2d) [%s] %s   <- %s"
                      % (e["n"], ln, tag, payload, ",".join(sorted(e["macs"]))))
        else:
            print("  (none captured)")
        print("  full log: %s" % LOGPATH)
        logf.close()
        s.close()


if __name__ == "__main__":
    main()
