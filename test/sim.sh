#!/usr/bin/env bash
# Builds and runs the interactive simulator: the real state machine, the real
# recogniser and the real renderer, on your terminal, driven by your keyboard.
#
# Needs a truecolour terminal. Nothing else: no curses, no SDL, no hardware.
set -euo pipefail
cd "$(dirname "$0")"

PANEL=../firmware/components/panel
UI=../firmware/components/ui
OUT=../build-host
mkdir -p "$OUT"

g++ -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter \
    -I"$PANEL/include" -I"$UI/include" \
    "$PANEL"/src/*.cpp "$UI"/src/*.cpp ../tools/sim.cpp -o "$OUT/sim"

exec "$OUT/sim" "$@"
