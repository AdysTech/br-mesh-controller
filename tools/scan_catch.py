#!/usr/bin/env python
# scan_catch.py — THE PROVEN catch (keyed 70:e7:f2:72:70:e8 on 2026-11-16).
# Full discovery+keying flow with a mid-scan power-cycle:
#   1. send `scan 12`  (WAKE + 12s discovery scan, then 1s delay, then SET_KEY+1s scan)
#   2. DURING the discovery scan, power OFF->ON the bulb (fresh boot) ~1s in
#   3. The bulb boots into the ongoing WAKE, advertises its factory-identity
#      frame, is detected (AddDeviceCallback), then keyed (AddLightCallback).
#
# Requires: pyserial.  Port: ESP_SERIAL_PORT=/dev/ttyACM0 ...
# Tasmota switch for the bulb under test (no LAN IP in VCS):
#   TASMOTA_URL="http://<ip>/cm?cmnd=%s" python tools/scan_catch.py
import os, sys, time, urllib.request
import serial

PORT = os.environ.get("ESP_SERIAL_PORT", "/dev/ttyACM0")
BAUD = 115200
TASMOTA = os.environ.get("TASMOTA_URL", "http://TASMOTA/cm?cmnd=%s")

def power(state):
    url = TASMOTA % ("Power%20" + state)
    try:
        with urllib.request.urlopen(url, timeout=3) as r:
            return r.read().decode()
    except Exception as e:
        return "ERR %s" % e

def main():
    s = serial.Serial(PORT, BAUD, timeout=0.5)
    time.sleep(0.05)
    s.DTR = False; s.RTS = True
    time.sleep(0.1)
    s.DTR = True; s.RTS = False
    time.sleep(0.1)
    s.reset_input_buffer()

    end = time.time() + 3
    while time.time() < end:
        l = s.readline()
        if l:
            sys.stdout.write(l.decode(errors="replace")); sys.stdout.flush()
    s.reset_input_buffer()

    s.write(b"scan 12\r\n")
    print(">> SENT: scan 12  (12s discovery + key, ~14s total)")
    # Discovery scan runs t=0..12. Power-cycle the bulb EARLY so it boots at
    # ~t=3 and has a full window to boot + advertise its identity frame.
    time.sleep(1.0)
    print(">> POWER OFF"); print("   ", power("OFF"))
    # Long enough that the bulb's capacitors drain and it COLD-boots (a short
    # gap cap-bridges and the bulb never re-enters its boot/pairing window).
    off_gap = float(os.environ.get("OFF_DELAY", "3.0"))
    print(">> (holding OFF %.1fs to drain caps)" % off_gap)
    time.sleep(off_gap)
    print(">> POWER ON");  print("   ", power("ON"))
    print(">> (bulb now boots into the ongoing WAKE; watch for detect + key)")

    end = time.time() + 12
    while time.time() < end:
        l = s.readline()
        if l:
            sys.stdout.write(l.decode(errors="replace")); sys.stdout.flush()
    s.close()

if __name__ == "__main__":
    main()
