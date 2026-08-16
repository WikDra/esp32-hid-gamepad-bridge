"""Sprawdza, ktory nimble_hidh.c poszedl do kompilacji: nasz czy ten z ESP-IDF.

Lokalna kopia komponentu esp_hid (firmware/components/esp_hid) zawiera latke na blad
opisany w AGENTS.md 4.27. Jesli build wziąłby wersje z IDF, latka nie dzialalaby,
a objaw wrocilby cicho - dlatego warto to sprawdzac po kazdej zmianie w drzewie.

    wsl python3 scripts/check_local_esp_hid.py
"""
import json
import sys

BUILD = "/mnt/d/wysypisko/esp32-hid-gamepad-bridge/firmware/build.esp32c3/compile_commands.json"
LOCAL = "/mnt/d/wysypisko/esp32-hid-gamepad-bridge/firmware/components/esp_hid"

entries = [e for e in json.load(open(BUILD)) if e["file"].endswith("nimble_hidh.c")]
if not entries:
    print("BLAD: nimble_hidh.c nie wystepuje w compile_commands.json")
    sys.exit(1)

ok = True
for e in entries:
    local = e["file"].startswith(LOCAL)
    print("%s  <- %s" % (e["file"], "LOKALNY (z latka)" if local else "Z IDF (BEZ LATKI!)"))
    ok = ok and local

sys.exit(0 if ok else 1)
