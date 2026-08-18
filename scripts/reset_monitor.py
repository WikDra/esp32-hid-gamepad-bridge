"""Resets the board and captures the log from the very first line (Windows side).

    python scripts\\reset_monitor.py [COM6] [seconds]
"""
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM6"
duration = float(sys.argv[2]) if len(sys.argv) > 2 else 20.0

ser = serial.Serial(port, 115200, timeout=0.5)
# USB Serial/JTAG: RTS drives the CHIP_EN line, so a short pulse resets the chip.
ser.setDTR(False)
ser.setRTS(True)
time.sleep(0.2)
ser.setRTS(False)

print(f"--- reset {port}, reading {duration:.0f}s ---", flush=True)
end = time.time() + duration
try:
    while time.time() < end:
        data = ser.read(4096)
        if data:
            sys.stdout.buffer.write(data)
            sys.stdout.flush()
finally:
    ser.close()
