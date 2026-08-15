"""Czyta konsole ESP32 przez chwile (strona Windows, bez idf.py).

    python scripts\\monitor.py [COM6] [sekundy]

Uwaga: na plytkach z natywnym USB (USB Serial/JTAG) linie DTR/RTS steruja resetem
i bootloaderem. pyserial domyslnie je aktywuje przy open(), co restartuje uklad -
a przy diagnozowaniu BLE restart w losowym momencie gubi kontekst (np. rozwala
trwajace parowanie). Dlatego port otwieramy z DTR/RTS ustawionymi na False.
Do celowego resetu jest scripts/reset_monitor.py.
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
# Musi byc ustawione PRZED open(), inaczej sterownik na chwile sciagnie reset.
ser.dtr = False
ser.rts = False

try:
    ser.open()
except Exception as exc:  # noqa: BLE001
    print(f"ERROR: nie moge otworzyc {port}: {exc}", flush=True)
    sys.exit(1)

print(f"--- czytam {port} przez {duration:.0f}s (bez resetu) ---", flush=True)
end = time.time() + duration
try:
    while time.time() < end:
        data = ser.read(4096)
        if data:
            sys.stdout.buffer.write(data)
            sys.stdout.flush()
finally:
    ser.close()
