#!/usr/bin/env python
# esp_serial.py — robust boot log capture for ESP32-S3 native USB.
import os, sys, time
import serial
PORT = os.environ.get("ESP_SERIAL_PORT", "/dev/ttyACM0")
BAUD = 115200

def wait_for_port(exists=True, timeout=15):
    """Wait for the serial port node to appear or disappear."""
    start = time.time()
    while time.time() - start < timeout:
        if os.path.exists(PORT) == exists:
            time.sleep(0.2)  # allow udev to settle
            return True
        time.sleep(0.1)
    return False

def stream_logs(duration):
    """Stream logs, automatically handling the S3's ROM-to-App USB handoff blip."""
    print("Waiting for USB stack stabilization...")
    time.sleep(0.8)  # Let the initial ROM bootloader state settle
    
    end = time.time() + duration
    while time.time() < end:
        try:
            s = serial.Serial(PORT, BAUD, timeout=0.5)
            print(f"Connected to {PORT}. Streaming boot logs...")
            while time.time() < end:
                line = s.readline()
                if line:
                    sys.stdout.write(line.decode(errors="replace"))
                    sys.stdout.flush()
            s.close()
            break
        except serial.SerialException:
            # If the port drops during the ROM-to-app handoff, catch it and reconnect
            try:
                s.close()
            except Exception:
                pass
            time.sleep(0.4)
            # Wait for it to come back if it disappeared
            if not os.path.exists(PORT):
                wait_for_port(exists=True, timeout=3)
            continue

def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "boot"

    if mode == "boot":
        dur = float(sys.argv[2]) if len(sys.argv) > 2 else 12
        print("Ready. Please press the physical RST button on your ESP32-S3 now...")
        
        # 1. If the port is currently active, wait for it to disconnect (RST pressed)
        if os.path.exists(PORT):
            wait_for_port(exists=False, timeout=5)
        
        print("Waiting for USB re-enumeration (/dev/ttyACM0)...")
        # 2. Wait for the port to come back online after reboot
        if not wait_for_port(exists=True, timeout=10):
            print(f"Timeout: Port {PORT} did not reappear.")
            sys.exit(1)
            
        stream_logs(dur)

    elif mode == "cmd":
        text = sys.argv[2]
        dur = float(sys.argv[3]) if len(sys.argv) > 3 else 4
        if not os.path.exists(PORT) and not wait_for_port(exists=True, timeout=5):
            print(f"Port {PORT} not available.")
            sys.exit(1)
            
        s = serial.Serial(PORT, BAUD, timeout=0.5)
        time.sleep(0.2)
        s.reset_input_buffer()
        s.write((text + "\r\n").encode())
        print(f"SENT: {text}")
        
        end = time.time() + dur
        while time.time() < end:
            line = s.readline()
            if line:
                sys.stdout.write(line.decode(errors="replace"))
                sys.stdout.flush()
        s.close()
    else:
        print("usage: esp_serial.py boot [sec] | cmd <text> [sec]")
        sys.exit(2)

if __name__ == "__main__":
    main()