#!/usr/bin/env bash
# Builds and runs the host-side tests and the preview renderer.
# Needs nothing but a C++17 compiler; no ESP-IDF, no hardware.
set -euo pipefail
cd "$(dirname "$0")"

PANEL=../firmware/components/panel
OUT=../build-host
mkdir -p "$OUT"

SRCS=("$PANEL"/src/*.cpp)
FLAGS=(-std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -I"$PANEL/include")

echo "building tests"
g++ "${FLAGS[@]}" "${SRCS[@]}" test_panel.cpp -o "$OUT/test_panel"

echo "building preview"
g++ "${FLAGS[@]}" "${SRCS[@]}" ../tools/preview.cpp -o "$OUT/preview"

"$OUT/test_panel"
