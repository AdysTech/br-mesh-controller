#!/usr/bin/env python
# nimble_probe.py — diagnostic: run a given ESP console command with a mid-window
# power-cycle and dump EVERYTHING, to tell us whether the NimBLE build's scan
# actually receives the bulb's frames (vs the Bluedroid build that could).
#
#   CMD="raw 10"   -> pure scan (no advertise): does the NimBLE RX see the bulb?
#   CMD="rawwake 12" -> WAKE advertise + concurrent scan: does WAKE+RX work?
#
# One OFF + one ON (no retries).  TASMOTA_URL="http://<ip>/cm?cmnd=%s"
import os, sys, time, urllib.request
import serial

PORT     = os.environ.get("ESP_SERIAL_PORT", "/dev/ttyACM0")
BAUD     = 115200
CMD      = os.environ.get("CMD", "rawwake 12")
WINDOW   = int(os.environ.get("WINDOW", "14"))
TASMOTA  = os.environ.get("TASMOTA_URL", "http://TASMOTA/cm?cmnd=%s")

def power(state):
    url = TASMOTA % ("Power%20" + state)
    try:
        with urllib.request.urlopen(url, timeout=3) as r:
            return r.read().decode()
    except Exception as e:
        return "ERR %s" % e

def main():
    s = serial.Serial(PORT, BAUD, timeout=0.4)
    time.sleep(0.05)
    s.reset_input_buffer()
    time.sleep(0.3)

    s.write((CMD + "\r\n").encode())
    print(">> SENT: %s   (window ~%ds)" % (CMD, WINDOW))
    time.sleep(1.0)
    print(">> POWER OFF"); print("   ", power("OFF"))
    off_gap = float(os.environ.get("OFF_DELAY", "3.0"))
    print(">> (holding OFF %.1fs to drain caps)" % off_gap)
    time.sleep(off_gap)
    print(">> POWER ON");  print("   ", power("ON"))
    print(">> (bulb boots now; watch for any 70:e7 frame)")
    print(">> " + "=" * 60)

    end = time.time() + WINDOW
    while time.time() < end:
        l = s.readline()
        if l:
            sys.stdout.write(l.decode(errors="replace"))
            sys.stdout.flush()
    print(">> " + "=" * 60)
    s.close()

if __name__ == "__main__":
    main()
