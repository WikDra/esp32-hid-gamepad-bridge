#!/usr/bin/env bash
# Firmware build in WSL.
#
#   ./scripts/build.sh [target] [extra idf.py arguments]
#   ./scripts/build.sh esp32c3
#   ./scripts/build.sh esp32c3 menuconfig
#
# Requires IDF >= 5.4.3 because of CONFIG_BT_NIMBLE_GATTC_AUTO_PAIR (AGENTS.md 4.1).
# Defaults to v5.5.1, the same version installed on the Windows side.
set -euo pipefail

TARGET="${1:-esp32c3}"
shift || true
IDF_DIR="${IDF_DIR:-$HOME/esp/v5.5.1/esp-idf}"
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../firmware" && pwd)"

if [ ! -f "$IDF_DIR/export.sh" ]; then
    echo "ERROR: no ESP-IDF found in $IDF_DIR" >&2
    echo "       set IDF_DIR=/path/to/esp-idf" >&2
    exit 1
fi

# A ~/.bashrc may pull in esp-matter (and with it an older IDF). We clear the state
# so that export.sh does not append itself to somebody else's environment.
unset IDF_PATH ESP_MATTER_PATH IDF_PYTHON_ENV_PATH || true
# shellcheck disable=SC1091
source "$IDF_DIR/export.sh" >/dev/null

IDF_VER="$(idf.py --version 2>/dev/null | tail -n1)"
echo "== IDF: $IDF_VER  ($IDF_PATH)"

cd "$PROJECT_DIR"
BUILD_DIR="build.$TARGET"
SDKCONFIG="sdkconfig.$TARGET"
DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.$TARGET"

# Local overrides - gitignored. Used to enable things we do not want in the repo,
# e.g. the diagnostic watchpoint (CONFIG_APP_DEBUG_WATCH_ADDR).
if [ -f "sdkconfig.local" ]; then
    DEFAULTS="$DEFAULTS;sdkconfig.local"
    echo "== using overrides from sdkconfig.local"
fi

echo "== target=$TARGET build_dir=$BUILD_DIR"
idf.py -B "$BUILD_DIR" -D SDKCONFIG="$SDKCONFIG" -D SDKCONFIG_DEFAULTS="$DEFAULTS" set-target "$TARGET"

if [ "$#" -gt 0 ]; then
    idf.py -B "$BUILD_DIR" -D SDKCONFIG="$SDKCONFIG" -D SDKCONFIG_DEFAULTS="$DEFAULTS" "$@"
else
    idf.py -B "$BUILD_DIR" -D SDKCONFIG="$SDKCONFIG" -D SDKCONFIG_DEFAULTS="$DEFAULTS" build
    echo
    echo "== binaries in $PROJECT_DIR/$BUILD_DIR"
    echo "   flash from Windows: scripts\\flash-win.bat COM6 $TARGET"
fi
