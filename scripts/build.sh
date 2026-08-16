#!/usr/bin/env bash
# Build firmware'u w WSL.
#
#   ./scripts/build.sh [target] [dodatkowe argumenty idf.py]
#   ./scripts/build.sh esp32c3
#   ./scripts/build.sh esp32c3 menuconfig
#
# Wymaga IDF >= 5.4.3 z powodu CONFIG_BT_NIMBLE_GATTC_AUTO_PAIR (AGENTS.md 4.1).
# Domyslnie v5.5.1, bo ta sama wersja jest zainstalowana po stronie Windows.
set -euo pipefail

TARGET="${1:-esp32c3}"
shift || true
IDF_DIR="${IDF_DIR:-$HOME/esp/v5.5.1/esp-idf}"
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../firmware" && pwd)"

if [ ! -f "$IDF_DIR/export.sh" ]; then
    echo "BLAD: nie znalazlem ESP-IDF w $IDF_DIR" >&2
    echo "      ustaw IDF_DIR=/sciezka/do/esp-idf" >&2
    exit 1
fi

# ~/.bashrc w tym WSL-u wciaga esp-matter (a z nim IDF v5.4.2). Czyscimy stan,
# zeby export.sh v5.5.1 nie doklejal sie do cudzego srodowiska.
unset IDF_PATH ESP_MATTER_PATH IDF_PYTHON_ENV_PATH || true
# shellcheck disable=SC1091
source "$IDF_DIR/export.sh" >/dev/null

IDF_VER="$(idf.py --version 2>/dev/null | tail -n1)"
echo "== IDF: $IDF_VER  ($IDF_PATH)"

cd "$PROJECT_DIR"
BUILD_DIR="build.$TARGET"
SDKCONFIG="sdkconfig.$TARGET"
DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.$TARGET"

# Lokalne nadpisania - gitignored. Sluzy do wlaczania rzeczy, ktorych nie chcemy
# w repo, np. diagnostycznego watchpointa (CONFIG_APP_DEBUG_WATCH_ADDR).
if [ -f "sdkconfig.local" ]; then
    DEFAULTS="$DEFAULTS;sdkconfig.local"
    echo "== uzywam nadpisan z sdkconfig.local"
fi

echo "== target=$TARGET build_dir=$BUILD_DIR"
idf.py -B "$BUILD_DIR" -D SDKCONFIG="$SDKCONFIG" -D SDKCONFIG_DEFAULTS="$DEFAULTS" set-target "$TARGET"

if [ "$#" -gt 0 ]; then
    idf.py -B "$BUILD_DIR" -D SDKCONFIG="$SDKCONFIG" -D SDKCONFIG_DEFAULTS="$DEFAULTS" "$@"
else
    idf.py -B "$BUILD_DIR" -D SDKCONFIG="$SDKCONFIG" -D SDKCONFIG_DEFAULTS="$DEFAULTS" build
    echo
    echo "== binarki w $PROJECT_DIR/$BUILD_DIR"
    echo "   wgranie z Windows: scripts\\flash-win.bat COM6 $TARGET"
fi
