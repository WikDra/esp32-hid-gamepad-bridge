"""Sprawdza, ktory nimble_hidh.c poszedl do kompilacji: nasz czy ten z ESP-IDF.

Lokalna kopia komponentu esp_hid (firmware/components/esp_hid) zawiera latke na blad
opisany w AGENTS.md 4.27. Jesli build wziąłby wersje z IDF, latka nie dzialalaby,
a objaw wrocilby cicho - dlatego warto to sprawdzac po kazdej zmianie w drzewie.

    python3 scripts/check_local_esp_hid.py [katalog_build]

Domyslnie sprawdza firmware/build.esp32c3, a jesli go nie ma - firmware/build.win.esp32c3
(build bez WSL). Sciezki wyliczane sa z polozenia tego pliku, wiec skrypt dziala
niezaleznie od tego, gdzie repo zostalo sklonowane i czy uruchamiamy go w WSL czy
w Windows.
"""
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
# Porownujemy po FRAGMENCIE sciezki, nie po prefiksie absolutnym. Powod praktyczny:
# compile_commands.json z buildu w WSL ma sciezki w konwencji /mnt/d/..., a ten sam
# skrypt moze byc uruchomiony w Windows, gdzie to samo repo to D:\... Porownanie
# absolutne dawalo wtedy falszywy negatyw.
LOCAL_MARK = "firmware/components/esp_hid"

if len(sys.argv) > 1:
    candidates = [Path(sys.argv[1])]
else:
    candidates = [ROOT / "firmware" / "build.esp32c3",
                  ROOT / "firmware" / "build.win.esp32c3"]

build = None
for c in candidates:
    if (c / "compile_commands.json").is_file():
        build = c / "compile_commands.json"
        break
if build is None:
    print("BLAD: nie znajduje compile_commands.json w:")
    for c in candidates:
        print("  " + str(c))
    print("Najpierw zbuduj projekt.")
    sys.exit(1)

entries = [e for e in json.loads(build.read_text()) if e["file"].endswith("nimble_hidh.c")]
if not entries:
    print("BLAD: nimble_hidh.c nie wystepuje w %s" % build)
    sys.exit(1)

ok = True
for e in entries:
    local = LOCAL_MARK in e["file"].replace("\\", "/")
    print("%s  <- %s" % (e["file"], "LOKALNY (z latka)" if local else "Z IDF (BEZ LATKI!)"))
    ok = ok and local

sys.exit(0 if ok else 1)
