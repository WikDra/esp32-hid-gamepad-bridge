"""Checks which nimble_hidh.c went into the build: ours or the one from ESP-IDF.

The local copy of the esp_hid component (firmware/components/esp_hid) carries a patch
for the bug described in AGENTS.md 4.27. If the build picked up the IDF version the
patch would be inert and the symptom would return silently - hence this check.

    python3 scripts/check_local_esp_hid.py [katalog_build]

By default it checks firmware/build.esp32c3, falling back to firmware/build.win.esp32c3
(the non-WSL build). Paths are derived from this file's location, so the script works
regardless of where the repo was cloned and whether it runs under WSL or on Windows.

"""
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
# We compare by a path FRAGMENT rather than an absolute prefix. Practical reason:
# compile_commands.json from a WSL build uses /mnt/... paths, while the same script may
# be run on Windows, where the same repo is a drive-letter path. An absolute
# comparison produced a false negative in that case.
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
    print("ERROR: no compile_commands.json found in:")
    for c in candidates:
        print("  " + str(c))
    print("Build the project first.")
    sys.exit(1)

entries = [e for e in json.loads(build.read_text()) if e["file"].endswith("nimble_hidh.c")]
if not entries:
    print("ERROR: nimble_hidh.c does not appear in %s" % build)
    sys.exit(1)

ok = True
for e in entries:
    local = LOCAL_MARK in e["file"].replace("\\", "/")
    print("%s  <- %s" % (e["file"], "LOCAL (patched)" if local else "FROM IDF (UNPATCHED!)"))
    ok = ok and local

sys.exit(0 if ok else 1)
