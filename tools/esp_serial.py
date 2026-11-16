#!/usr/bin/env python
# esp_serial.py — low-level ESP32 serial plumbing for the BRMesh POC.
#   esp_serial.py boot [seconds]   reset the chip via RTS and capture boot log
#   esp_serial.py cmd <text> [sec] send one console line, read the response
#
# Requires: pyserial  (pip install pyserial)
# Port is overridable: ESP_SERIAL_PORT=/dev/ttyACM1 ...  (it can swap on power-cycle)
import os, sys, time
import serial

PORT = os.environ.get("ESP_SERIAL_PORT", "/dev/ttyACM0")
BAUD = 115200

def open_port():
    s = serial.Serial(PORT, BAUD, timeout=0.5)
    time.sleep(0.05)
    # ESP32-S3 USB-Serial/JTAG: DTR -> EN (reset), RTS -> IO0 (boot mode).
    # Hold reset with IO0 high (app boot), then release by raising DTR.
    s.DTR = False
    s.RTS = True
    time.sleep(0.1)
    s.DTR = True           # release reset -> chip boots
    s.RTS = False
    time.sleep(0.05)
    s.reset_input_buffer()
    return s

def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "boot"
    s = open_port()
    if mode == "boot":
        dur = float(sys.argv[2]) if len(sys.argv) > 2 else 12
    elif mode == "cmd":
        text = sys.argv[2]
        dur = float(sys.argv[3]) if len(sys.argv) > 3 else 4
        time.sleep(0.2)
        s.reset_input_buffer()
        s.write((text + "\r\n").encode())
        print(f"SENT: {text}")
    else:
        print("usage: esp_serial.py boot [sec] | cmd <text> [sec]")
        sys.exit(2)

    end = time.time() + dur
    while time.time() < end:
        line = s.readline()
        if line:
            sys.stdout.write(line.decode(errors="replace"))
            sys.stdout.flush()
    s.close()

if __name__ == "__main__":
    main()
