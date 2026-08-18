"""Reads the ESP32 console for a while (Windows side, without idf.py).

    python scripts\\monitor.py [COM6] [seconds]

Note: on boards with native USB (USB Serial/JTAG) the DTR/RTS lines drive reset and
the bootloader. pyserial asserts them on open() by default, which reboots the chip -
and when debugging BLE a reboot at a random moment loses context (it can tear down
an in-progress pairing). So the port is opened with DTR/RTS set to False.
For a deliberate reset there is scripts/reset_monitor.py.
"""
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM6"
duration = float(sys.argv[2]) if len(sys.argv) > 2 else 20.0

ser = serial.Serial()
ser.port = port
ser.baudrate = 115200
ser.timeout = 0.5
# Must be set BEFORE open(), otherwise the driver briefly pulls the reset line.
ser.dtr = False
ser.rts = False

try:
    ser.open()
except Exception as exc:  # noqa: BLE001
    print(f"ERROR: cannot open {port}: {exc}", flush=True)
    sys.exit(1)

print(f"--- reading {port} for {duration:.0f}s (no reset) ---", flush=True)
end = time.time() + duration
try:
    while time.time() < end:
        data = ser.read(4096)
        if data:
            sys.stdout.buffer.write(data)
            sys.stdout.flush()
finally:
    ser.close()
