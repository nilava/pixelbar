#!/usr/bin/env bash
# Builds and runs the host-side tests, the preview renderer and the simulator.
# Needs nothing but a C++17 compiler; no ESP-IDF, no hardware.
#
# Both firmware components built here are deliberately free of ESP-IDF. If a
# source is ever added to either that plain g++ cannot compile, this script
# breaks, which is the point: it is the thing that keeps the drawing and input
# layers portable and therefore testable.
set -euo pipefail
cd "$(dirname "$0")"

PANEL=../firmware/components/panel
UI=../firmware/components/ui
OUT=../build-host
mkdir -p "$OUT"

SRCS=("$PANEL"/src/*.cpp "$UI"/src/*.cpp)
FLAGS=(-std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter
       -I"$PANEL/include" -I"$UI/include")

echo "building tests"
g++ "${FLAGS[@]}" "${SRCS[@]}" test_panel.cpp test_ui.cpp -o "$OUT/test_panel"

echo "building preview"
g++ "${FLAGS[@]}" "${SRCS[@]}" ../tools/preview.cpp -o "$OUT/preview"

"$OUT/test_panel"
