#!/usr/bin/env python3
"""Generuje firmware/main/xbox_report_map.h z deskryptora pada Xbox.

Dlaczego skryptem, a nie recznie: Windows dobiera sterownik XInput po tozsamosci
urzadzenia i deskryptorze raportu, wiec deskryptor musi byc BAJT W BAJT taki, jak
w prawdziwym padzie. Przepisywanie 300 bajtow z komentarzami reka to gwarantowana
literowka, ktorej potem szukalibysmy w zachowaniu Windows, a nie w kodzie.

Zrodlo: https://github.com/Mystfit/ESP32-BLE-CompositeHID (licencja MIT),
plik XboxDescriptors.h.

Uzycie (repo referencyjne trafia do .ref/, ktory jest gitignorowany):

    git clone --depth 1 https://github.com/Mystfit/ESP32-BLE-CompositeHID .ref
    python3 scripts/gen_xbox_report_map.py .ref/XboxDescriptors.h [nazwa_tablicy]

Domyslna tablica to XboxOneS_1914_HIDDescriptor (pad Xbox Series X, model 1914,
PID 0x0B13). WAZNE: to nie jest dowolny wybor. Sterownik XInput dla BLE w Windows
(C:\\Windows\\INF\\xinputhid.inf, sekcja Btle_Bus = "Bluetooth LE XINPUT compatible
input device") wiaze sie WYLACZNIE z PID z rodziny 0x0Bxx:

    VID&02045e_PID&0b13, 0b20, 0b21, 0b22, 0b23, 0b24, 0b25, 0b26, 0b27

Pada Xbox One S (model 1708, PID 0x02FD) na tej liscie NIE MA - przy tamtej
tozsamosci Windows podpina generyczny sterownik HID i gry na XInput pada nie widza.
Szczegoly: AGENTS.md 4.32.

Wygenerowany naglowek jest commitowany, wiec do zwyklego budowania repo
referencyjne nie jest potrzebne.
"""

import hashlib
import re
import sys
from pathlib import Path

DEFAULT_ARRAY = "XboxOneS_1914_HIDDescriptor"

# Nazwane identyfikatory raportow z naglowka zrodlowego.
REPORT_IDS = {
    "XBOX_INPUT_REPORT_ID": 0x01,
    "XBOX_EXTRA_INPUT_REPORT_ID": 0x02,
    "XBOX_OUTPUT_REPORT_ID": 0x03,
    "XBOX_EXTRA_OUTPUT_REPORT_ID": 0x04,
}


def extract_bytes(src_text, array):
    """Wyciaga tablice bajtow deskryptora ze zrodla C."""
    start = src_text.find(array)
    if start < 0:
        raise SystemExit(f"nie znalazlem tablicy {array} w pliku zrodlowym")
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


def parse_reports(data):
    """Przechodzi deskryptor i liczy dlugosc kazdego raportu z sumy bitow pol.

    Dzieki temu dlugosci w kodzie C nie moga sie rozjechac z deskryptorem - a
    rozjechanie sie akurat tego jest trudne do zauwazenia, bo host po prostu
    dostaje raport o innej dlugosci niz sie spodziewa.

    Zwraca liste (report_id, typ, dlugosc_w_bajtach) w kolejnosci wystapienia,
    gdzie typ to 1 = INPUT, 2 = OUTPUT, 3 = FEATURE (tak jak w deskryptorze
    Report Reference 0x2908).
    """
    MAIN_INPUT, MAIN_OUTPUT, MAIN_FEATURE = 0x08, 0x09, 0x0B
    GLOBAL_REPORT_SIZE, GLOBAL_REPORT_ID, GLOBAL_REPORT_COUNT = 0x07, 0x08, 0x09

    report_size = 0
    report_count = 0
    report_id = 0
    depth = 0
    bits = {}
    order = []

    i = 0
    while i < len(data):
        item = data[i]
        size = item & 0x03
        size = 4 if size == 3 else size
        itype = (item >> 2) & 0x03
        tag = (item >> 4) & 0x0F

        value = 0
        for k in range(size):
            value |= data[i + 1 + k] << (8 * k)

        if itype == 0:  # Main
            if tag == 0x0A:
                depth += 1
            elif tag == 0x0C:
                depth -= 1
            elif tag in (MAIN_INPUT, MAIN_OUTPUT, MAIN_FEATURE):
                kind = {MAIN_INPUT: 1, MAIN_OUTPUT: 2, MAIN_FEATURE: 3}[tag]
                key = (report_id, kind)
                if key not in bits:
                    bits[key] = 0
                    order.append(key)
                bits[key] += report_size * report_count
        elif itype == 1:  # Global
            if tag == GLOBAL_REPORT_SIZE:
                report_size = value
            elif tag == GLOBAL_REPORT_COUNT:
                report_count = value
            elif tag == GLOBAL_REPORT_ID:
                report_id = value

        i += 1 + size

    if i != len(data):
        raise SystemExit(f"deskryptor niespojny: parsowanie skonczylo na {i}, dlugosc {len(data)}")
    if depth != 0:
        raise SystemExit(f"deskryptor niespojny: bilans kolekcji {depth}")

    out = []
    for key in order:
        nbits = bits[key]
        if nbits % 8:
            raise SystemExit(f"raport 0x{key[0]:02X} typ {key[1]} ma {nbits} bitow, nie wielokrotnosc 8")
        out.append((key[0], key[1], nbits // 8))
    return out


def main():
    if len(sys.argv) not in (2, 3):
        raise SystemExit(__doc__)
    src = Path(sys.argv[1])
    array = sys.argv[2] if len(sys.argv) == 3 else DEFAULT_ARRAY
    data = extract_bytes(src.read_text(encoding="utf-8", errors="replace"), array)
    reports = parse_reports(data)
    digest = hashlib.sha256(bytes(data)).hexdigest()

    lines = []
    for i in range(0, len(data), 12):
        chunk = ", ".join(f"0x%02X" % b for b in data[i:i + 12])
        lines.append("    " + chunk + ",")
    lines[-1] = lines[-1].rstrip(",")

    kind_name = {1: "INPUT", 2: "OUTPUT", 3: "FEATURE"}
    rpt_lines = []
    for rid, kind, length in reports:
        rpt_lines.append(f"    {{ 0x{rid:02X}, {kind}, {length:2d} }},"
                         f"  /* {kind_name[kind]:7s} */")
    rpt_summary = ", ".join(f"0x{r:02X} {kind_name[k]} {n} B" for r, k, n in reports)

    input_len = next((n for r, k, n in reports if k == 1), 0)

    header = f"""/*
 * PLIK GENEROWANY - nie edytowac recznie.
 * Zrodlo: scripts/gen_xbox_report_map.py {src.name} {array}
 *
 * Deskryptor raportu HID bezprzewodowego pada Xbox. Pochodzi z projektu
 * Mystfit/ESP32-BLE-CompositeHID na licencji MIT, ktory odczytal go z prawdziwego
 * pada.
 *
 * Windows dobiera sterownik XInput po tozsamosci (PnP ID) I deskryptorze. Kazda
 * zmiana tych bajtow wymaga usuniecia pada z listy urzadzen Bluetooth w Windows
 * i sparowania od nowa (AGENTS.md 4.7).
 *
 * Raporty policzone z deskryptora: {rpt_summary}
 * sha256 zawartosci: {digest}
 */

#pragma once

#include <stdint.h>

/* Dlugosc raportu wejsciowego pada, policzona z sumy bitow pol w deskryptorze. */
#define XBOX_INPUT_REPORT_LEN {input_len}

/* Raporty zadeklarowane w deskryptorze, w kolejnosci wystapienia.
 * typ: 1 = INPUT, 2 = OUTPUT, 3 = FEATURE (jak w Report Reference 0x2908). */
struct xbox_report_info {{
    uint8_t id;
    uint8_t type;
    uint8_t len;
}};

static const struct xbox_report_info xbox_reports[] = {{
{chr(10).join(rpt_lines)}
}};

static const uint8_t xbox_report_map[] = {{
{chr(10).join(lines)}
}};
"""
    out = Path(__file__).resolve().parent.parent / "firmware" / "main" / "xbox_report_map.h"
    out.write_text(header, encoding="utf-8")
    print(f"zapisano {out}")
    print(f"tablica zrodlowa: {array}")
    print(f"dlugosc deskryptora: {len(data)} B")
    print(f"raporty: {rpt_summary}")
    print(f"sha256: {digest}")


if __name__ == "__main__":
    main()
