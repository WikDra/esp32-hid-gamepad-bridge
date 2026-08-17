#!/usr/bin/env python3
"""Generuje firmware/main/xbox_report_map.h z deskryptora pada Xbox.

Dlaczego skryptem, a nie recznie: Windows dobiera sterownik XInput po tozsamosci
urzadzenia i deskryptorze raportu, wiec deskryptor musi byc BAJT W BAJT taki, jak
w prawdziwym padzie. Przepisywanie 300 bajtow z komentarzami reka to gwarantowana
literowka, ktorej potem szukalibysmy w zachowaniu Windows, a nie w kodzie.

Zrodlo: https://github.com/Mystfit/ESP32-BLE-CompositeHID (licencja MIT),
plik XboxDescriptors.h, tablica XboxOneS_1708_HIDDescriptor.

Uzycie (repo referencyjne trafia do .ref/, ktory jest gitignorowany):

    git clone --depth 1 https://github.com/Mystfit/ESP32-BLE-CompositeHID .ref
    python3 scripts/gen_xbox_report_map.py .ref/XboxDescriptors.h

Wygenerowany naglowek jest commitowany, wiec do zwyklego budowania repo
referencyjne nie jest potrzebne.
"""

import hashlib
import re
import sys
from pathlib import Path

ARRAY = "XboxOneS_1708_HIDDescriptor"

# Nazwane identyfikatory raportow z naglowka zrodlowego.
REPORT_IDS = {
    "XBOX_INPUT_REPORT_ID": 0x01,
    "XBOX_EXTRA_INPUT_REPORT_ID": 0x02,
    "XBOX_OUTPUT_REPORT_ID": 0x03,
    "XBOX_EXTRA_OUTPUT_REPORT_ID": 0x04,
}


def extract_bytes(src_text):
    """Wyciaga tablice bajtow deskryptora ze zrodla C."""
    start = src_text.find(ARRAY)
    if start < 0:
        raise SystemExit(f"nie znalazlem tablicy {ARRAY} w pliku zrodlowym")
    brace = src_text.find("{", start)
    if brace < 0:
        raise SystemExit("nie znalazlem otwierajacego nawiasu tablicy")

    depth = 0
    end = None
    for i in range(brace, len(src_text)):
        if src_text[i] == "{":
            depth += 1
        elif src_text[i] == "}":
            depth -= 1
            if depth == 0:
                end = i
                break
    if end is None:
        raise SystemExit("nie znalazlem zamykajacego nawiasu tablicy")

    body = src_text[brace + 1:end]
    body = re.sub(r"//[^\n]*", "", body)          # komentarze liniowe
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)  # komentarze blokowe

    out = []
    for tok in body.split(","):
        tok = tok.strip()
        if not tok:
            continue
        if tok in REPORT_IDS:
            out.append(REPORT_IDS[tok])
        elif re.fullmatch(r"0[xX][0-9a-fA-F]+", tok):
            out.append(int(tok, 16))
        elif re.fullmatch(r"\d+", tok):
            out.append(int(tok))
        else:
            raise SystemExit(f"nieznany token w tablicy: {tok!r}")

    for b in out:
        if not 0 <= b <= 0xFF:
            raise SystemExit(f"bajt poza zakresem: {b}")
    return out


def validate(data):
    """Prosta kontrola spojnosci deskryptora: bilans kolekcji i obecnosc raportow."""
    depth = 0
    i = 0
    report_ids = set()
    while i < len(data):
        item = data[i]
        size = item & 0x03
        size = 4 if size == 3 else size
        tag = item & 0xFC
        if tag == 0xA0:      # COLLECTION
            depth += 1
        elif tag == 0xC0:    # END_COLLECTION
            depth -= 1
        elif tag == 0x84:    # REPORT_ID
            report_ids.add(data[i + 1])
        i += 1 + size
    if i != len(data):
        raise SystemExit(f"deskryptor niespojny: parsowanie skonczylo na {i}, dlugosc {len(data)}")
    if depth != 0:
        raise SystemExit(f"deskryptor niespojny: bilans kolekcji {depth}")
    return sorted(report_ids)


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    src = Path(sys.argv[1])
    data = extract_bytes(src.read_text(encoding="utf-8", errors="replace"))
    report_ids = validate(data)
    digest = hashlib.sha256(bytes(data)).hexdigest()

    lines = []
    for i in range(0, len(data), 12):
        chunk = ", ".join(f"0x%02X" % b for b in data[i:i + 12])
        lines.append("    " + chunk + ",")
    lines[-1] = lines[-1].rstrip(",")

    header = f"""/*
 * PLIK GENEROWANY - nie edytowac recznie.
 * Zrodlo: scripts/gen_xbox_report_map.py {src.name} (tablica {ARRAY})
 *
 * Deskryptor raportu HID pada Xbox One S (model 1708, firmware sprzed 2021).
 * Pochodzi z projektu Mystfit/ESP32-BLE-CompositeHID na licencji MIT, ktory
 * odczytal go z prawdziwego pada.
 *
 * Windows dobiera sterownik XInput po tozsamosci (PnP ID) I deskryptorze, wiec
 * te bajty musza byc dokladnie takie. Kazda zmiana wymaga usuniecia pada z listy
 * urzadzen Bluetooth w Windows i sparowania od nowa (AGENTS.md 4.7).
 *
 * Raporty w deskryptorze: {', '.join(f'0x{r:02X}' for r in report_ids)}
 * sha256 zawartosci: {digest}
 */

#pragma once

#include <stdint.h>

#define XBOX_RPT_ID_INPUT        0x01  /* pad: osie, spusty, hat, przyciski (16 B) */
#define XBOX_RPT_ID_CONSUMER     0x02  /* przycisk Guide/Xbox, Consumer Page (1 B)  */
#define XBOX_RPT_ID_RUMBLE       0x03  /* OUTPUT: wibracje (8 B)                    */
#define XBOX_RPT_ID_BATTERY      0x04  /* INPUT: Battery Strength (1 B)             */

static const uint8_t xbox_report_map[] = {{
{chr(10).join(lines)}
}};
"""
    out = Path(__file__).resolve().parent.parent / "firmware" / "main" / "xbox_report_map.h"
    out.write_text(header, encoding="utf-8")
    print(f"zapisano {out}")
    print(f"dlugosc deskryptora: {len(data)} B (limit REPORT_MAP_SIZE w NimBLE: 512 B)")
    print(f"raporty: {', '.join(f'0x{r:02X}' for r in report_ids)}")
    print(f"sha256: {digest}")


if __name__ == "__main__":
    main()
