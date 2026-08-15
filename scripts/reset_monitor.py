"""Resetuje plytke i lapie log od pierwszej linii (strona Windows).

    python scripts\\reset_monitor.py [COM6] [sekundy]
"""
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM6"
duration = float(sys.argv[2]) if len(sys.argv) > 2 else 20.0

ser = serial.Serial(port, 115200, timeout=0.5)
# USB Serial/JTAG: RTS steruje linia CHIP_EN, wiec krotki impuls resetuje uklad.
ser.setDTR(False)
ser.setRTS(True)
time.sleep(0.2)
ser.setRTS(False)

print(f"--- reset {port}, czytam {duration:.0f}s ---", flush=True)
end = time.time() + duration
try:
    while time.time() < end:
        data = ser.read(4096)
        if data:
            sys.stdout.buffer.write(data)
            sys.stdout.flush()
finally:
    ser.close()
