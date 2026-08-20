#!/usr/bin/env python3
"""
Decodes the hex blocks that esp_ble_controller_log_dump_all() prints on ESP32-C6/H2.

WHY THIS EXISTS. Every conclusion about AGENTS.md 4.35 rested on NimBLE's "Connection failed;
status=13", which is a HOST-side timeout: it says our host gave up, and nothing at all about
what the controller did in those seconds. The controller keeps its own log, HCI traffic
included, and can dump it - but it dumps raw hex, so it needs reading.

The dump lines look like this (one record, wrapped in the console):

    23 00  4c 46 61 01  02 00  4a 20  3e 17 02 01 00 01 e3 af df 8e 1c fb 0b 02 01 04 ...
    |      |            |      |      |
    |      |            |      |      +-- the HCI packet: 0x3e = LE Meta Event, len 0x17,
    |      |            |      |          subevent 0x02 = LE Advertising Report, ...
    |      |            |      +-- record type/source marker
    |      |            +-- unknown 16-bit field
    |      +-- 32-bit timestamp
    +-- 16-bit record length

The framing around the HCI packet is not documented, so this script does not pretend to know
it: it scans each record for the HCI structures we actually care about and reports those, which
is enough to answer "did the controller get told to connect, and did it ever answer".

Usage:  python scripts/decode_ctrl_log.py <captured console log>
"""
import re
import sys

# HCI bits we care about for the connection-establishment question.
LE_META = 0x3E
SUB_CONN_COMPLETE = 0x01
SUB_ADV_REPORT = 0x02
SUB_ENH_CONN_COMPLETE = 0x0A
OGF_OCF_CREATE_CONN = (0x0D, 0x20)      # LE_Create_Connection, opcode 0x200D
OGF_OCF_CREATE_CONN_CANCEL = (0x0E, 0x20)  # LE_Create_Connection_Cancel, opcode 0x200E
OGF_OCF_EXT_CREATE_CONN = (0x43, 0x20)  # LE_Extended_Create_Connection, opcode 0x2043

ADV_EVT = {0x00: "ADV_IND", 0x01: "ADV_DIRECT_IND", 0x02: "ADV_SCAN_IND",
           0x03: "ADV_NONCONN_IND", 0x04: "SCAN_RSP"}


def addr(b):
    return ":".join("%02x" % x for x in reversed(b))


def decode(data, where):
    """Reports every HCI structure of interest found in one record."""
    out = []
    for i in range(len(data) - 3):
        # LE Meta Event
        if data[i] == LE_META:
            plen, sub = data[i + 1], data[i + 2]
            if sub == SUB_ADV_REPORT and i + 4 + 7 <= len(data):
                evt = data[i + 4]
                atype = data[i + 5]
                a = data[i + 6:i + 12]
                dlen = data[i + 12] if i + 12 < len(data) else 0
                rssi = data[i + 13 + dlen] if i + 13 + dlen < len(data) else None
                out.append("ADV report  %-14s type=%d %s data=%dB rssi=%s"
                           % (ADV_EVT.get(evt, "?%02x" % evt), atype, addr(a), dlen,
                              "%d" % (rssi - 256) if rssi and rssi > 127 else rssi))
            elif sub in (SUB_CONN_COMPLETE, SUB_ENH_CONN_COMPLETE):
                status = data[i + 3]
                out.append("*** CONNECTION COMPLETE  subevent=0x%02x status=0x%02x  (len=%d)"
                           % (sub, status, plen))
        # HCI command: the opcode is two bytes, little endian
        pair = (data[i], data[i + 1])
        if pair == OGF_OCF_CREATE_CONN:
            out.append("*** CMD LE_Create_Connection  params=%s"
                       % " ".join("%02x" % x for x in data[i + 2:i + 28]))
        elif pair == OGF_OCF_CREATE_CONN_CANCEL:
            out.append("*** CMD LE_Create_Connection_CANCEL")
        elif pair == OGF_OCF_EXT_CREATE_CONN:
            out.append("*** CMD LE_Extended_Create_Connection")
    return out


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1

    text = open(sys.argv[1], "r", encoding="utf-8", errors="replace").read()
    # The console wraps records and decorates them with colour escapes; keep only hex pairs.
    text = re.sub(r"\x1b\[[0-9;]*m", "", text)

    interesting = 0
    for lineno, line in enumerate(text.splitlines(), 1):
        pairs = re.findall(r"\b([0-9a-f]{2})\b", line)
        if len(pairs) < 8:
            continue
        data = bytes(int(p, 16) for p in pairs)
        for msg in decode(data, lineno):
            print("line %5d  %s" % (lineno, msg))
            interesting += 1

    print("\n%d structures of interest found" % interesting)
    return 0


if __name__ == "__main__":
    sys.exit(main())
