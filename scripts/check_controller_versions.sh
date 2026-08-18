#!/usr/bin/env bash
# One-off check: which BLE controller library version does each ESP-IDF release point at?
# The controller is a prebuilt blob delivered as a git submodule, so the submodule commit is
# the only way to compare versions without downloading the whole release.
set -u

echo "=== v6.0.2, from the GitHub API ==="
curl -sL "https://api.github.com/repos/espressif/esp-idf/contents/components/bt/controller?ref=v6.0.2" \
  | python3 -c 'import json,sys
d=json.load(sys.stdin)
for e in d:
    if e["name"].startswith("lib_esp32"):
        print("%-24s %-10s %s" % (e["name"], e["type"], e.get("sha","")[:12]))'

echo
echo "=== v5.5.1, locally ==="
cd "$HOME/esp/v5.5.1/esp-idf" || exit 1
git submodule status components/bt/controller/lib_esp32h2 \
                      components/bt/controller/lib_esp32c6 \
                      components/bt/controller/lib_esp32c3_family 2>/dev/null \
  | sed 's/^[ +-]//' | awk '{printf "%-24s %s\n", $2, substr($1,1,12)}'
