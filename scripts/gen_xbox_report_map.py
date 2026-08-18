#!/usr/bin/env python3
"""Generates firmware/main/xbox_report_map.h from the Xbox pad descriptor.

Why a script rather than by hand: Windows picks the XInput driver based on the device
identity and the report descriptor, so the descriptor has to be BYTE FOR BYTE what a
real pad sends. Retyping 300 commented bytes by hand guarantees a typo that would
then be hunted in the behaviour of Windows rather than in the code.

Zrodlo: https://github.com/Mystfit/ESP32-BLE-CompositeHID (licencja MIT),
plik XboxDescriptors.h.

Usage (the reference repo goes into .ref/, which is gitignored):

    git clone --depth 1 https://github.com/Mystfit/ESP32-BLE-CompositeHID .ref
    python3 scripts/gen_xbox_report_map.py .ref/XboxDescriptors.h [nazwa_tablicy]

Domyslna tablica to XboxOneS_1914_HIDDescriptor (pad Xbox Series X, model 1914,
PID 0x0B13). IMPORTANT: this is not an arbitrary choice. Windows' XInput driver for
(C:\\Windows\\INF\\xinputhid.inf, sekcja Btle_Bus = "Bluetooth LE XINPUT compatible
input device") binds ONLY to PIDs from the 0x0Bxx family:

    VID&02045e_PID&0b13, 0b20, 0b21, 0b22, 0b23, 0b24, 0b25, 0b26, 0b27

The Xbox One S pad (model 1708, PID 0x02FD) is NOT on that list - with that identity
Windows attaches the generic HID driver and XInput games do not see the pad at all.
Szczegoly: AGENTS.md 4.32.

The generated header is committed, so a normal build does not need the reference
repository.
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
    """Extracts the descriptor byte array from the C source."""
    start = src_text.find(array)
    if start < 0:
        raise SystemExit(f"array {array} not found in the source file")
    brace = src_text.find("{", start)
    if brace < 0:
        raise SystemExit("opening brace of the array not found")

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
        raise SystemExit("closing brace of the array not found")

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
            raise SystemExit(f"unknown token in the array: {tok!r}")

    for b in out:
        if not 0 <= b <= 0xFF:
            raise SystemExit(f"byte out of range: {b}")
    return out


def parse_reports(data):
    """Przechodzi deskryptor i liczy dlugosc kazdego raportu z sumy bitow pol.

    This keeps the lengths in the C code from drifting away from the descriptor - and
    that particular drift is hard to spot, because the host simply receives a report of
    a different length than it expects.

    Returns a list of (report_id, type, length_in_bytes) in order of appearance,
    where type is 1 = INPUT, 2 = OUTPUT, 3 = FEATURE (as in the
    Report Reference descriptor 0x2908).
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
        raise SystemExit(f"inconsistent descriptor: parsing ended at {i}, length {len(data)}")
    if depth != 0:
        raise SystemExit(f"inconsistent descriptor: collection balance {depth}")

    out = []
    for key in order:
        nbits = bits[key]
        if nbits % 8:
            raise SystemExit(f"report 0x{key[0]:02X} type {key[1]} has {nbits} bits, not a multiple of 8")
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
 * GENERATED FILE - do not edit by hand.
 * Zrodlo: scripts/gen_xbox_report_map.py {src.name} {array}
 *
 * HID report descriptor of a wireless Xbox controller. Taken from the project
 * Mystfit/ESP32-BLE-CompositeHID under the MIT licence, which read it from a real
 * pad.
 *
 * Windows picks the XInput driver based on the identity (PnP ID) AND the descriptor.
 * Any change to these bytes requires removing the pad from the Windows Bluetooth
 * device list and pairing again (AGENTS.md 4.7).
 *
 * Reports computed from the descriptor: {rpt_summary}
 * sha256 of the contents: {digest}
 */

#pragma once

#include <stdint.h>

/* Pad input report length, computed from the sum of field bits in the descriptor. */
#define XBOX_INPUT_REPORT_LEN {input_len}

/* Reports declared in the descriptor, in order of appearance.
 * type: 1 = INPUT, 2 = OUTPUT, 3 = FEATURE (as in Report Reference 0x2908). */
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
    print(f"written {out}")
    print(f"source array: {array}")
    print(f"descriptor length: {len(data)} B")
    print(f"reports: {rpt_summary}")
    print(f"sha256: {digest}")


if __name__ == "__main__":
    main()
