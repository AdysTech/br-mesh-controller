#!/usr/bin/env python
# rawwake_catch.py — WAKE broadcast + duplicate raw scan + mid-scan power-cycle.
# Decisive test: does a WAKE trigger the bulb's factory-identity frame?
# (It does — this is what proved the identity frame is WAKE-gated.)
#
# Requires: pyserial.  Port: ESP_SERIAL_PORT=/dev/ttyACM0 ...
# Tasmota switch for the bulb under test (no LAN IP in VCS):
#   TASMOTA_URL="http://<ip>/cm?cmnd=%s" python tools/rawwake_catch.py
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

    s.write(b"rawwake 25\r\n")
    print(">> SENT: rawwake 25  (WAKE broadcast + every frame)")
    time.sleep(3.0)

    print(">> POWER OFF"); print("   ", power("OFF"))
    # Gap must be long enough that the bulb's capacitors fully drain and it
    # COLD-boots (a short gap cap-bridges and it never reverts to factory).
    off_gap = float(os.environ.get("OFF_DELAY", "3.0"))
    print(">> (holding OFF %.1fs to drain caps)" % off_gap)
    time.sleep(off_gap)
    print(">> POWER ON");  print("   ", power("ON"))

    end = time.time() + 24
    while time.time() < end:
        l = s.readline()
        if l:
            sys.stdout.write(l.decode(errors="replace")); sys.stdout.flush()
    s.close()

if __name__ == "__main__":
    main()
