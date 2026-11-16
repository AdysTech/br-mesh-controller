#!/usr/bin/env python
# raw_catch.py — "mirror the phone" raw capture.
# A plain BLE scan (NO WAKE/advertising), with the bulb power-cycled mid-scan.
# The bulb only advertises a few seconds after fresh boot; the clean scan
# catches it the way the phone does and dumps the raw manufacturer bytes so we
# can see exactly what it sends. This is how the 18-byte factory-identity
# frame (CLAUDE.md §4.6) was first observed.
#
# Requires: pyserial.  Port: ESP_SERIAL_PORT=/dev/ttyACM0 ...
# Tasmota switch for the bulb under test (no LAN IP in VCS):
#   TASMOTA_URL="http://<ip>/cm?cmnd=%s" python tools/raw_catch.py
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

    # drain boot log
    end = time.time() + 3
    while time.time() < end:
        l = s.readline()
        if l:
            sys.stdout.write(l.decode(errors="replace")); sys.stdout.flush()
    s.reset_input_buffer()

    s.write(b"raw 25\r\n")
    print(">> SENT: raw 25  (clean scan, no WAKE)")
    time.sleep(3.0)

    print(">> POWER OFF"); print("   ", power("OFF"))
    time.sleep(2.0)
    print(">> POWER ON");  print("   ", power("ON"))

    end = time.time() + 24
    while time.time() < end:
        l = s.readline()
        if l:
            sys.stdout.write(l.decode(errors="replace")); sys.stdout.flush()
    s.close()

if __name__ == "__main__":
    main()
