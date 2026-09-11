// Logical panel geometry and the logical -> LED-index mapping.
//
// Logical coordinates are what patterns draw in: x runs 0..23 left to right as
// you face the panel, y runs 0..7 top to bottom. Row 0 is the top.
//
// Physically the panel is three 8x8 WS2812B boards chained left to right. How
// the LEDs are wired *inside* one board varies between suppliers, so the wiring
// is described by a Wiring struct rather than baked in. Run Pattern::MapTest on
// the real hardware once and set kWiring in config.h to match.
#pragma once
#include <cstdint>

namespace panel {

constexpr int kTileW = 8;
constexpr int kTileH = 8;
constexpr int kTiles = 3;
constexpr int kWidth = kTileW * kTiles;   // 24
constexpr int kHeight = kTileH;           // 8
constexpr int kNumLeds = kWidth * kHeight; // 192

// Which corner of a board holds LED 0.
enum class Corner : uint8_t { TopLeft, TopRight, BottomLeft, BottomRight };

// Whether the index advances along a row or down a column first.
enum class Axis : uint8_t { Row, Column };

struct Wiring {
  Corner first = Corner::TopLeft;
  Axis major = Axis::Row;
  // Serpentine ("boustrophedon") boards reverse direction on every second
  // line. Progressive boards always run the same way.
  bool serpentine = true;
  // True if the data chain enters at the right-hand board instead of the left.
  bool chainRightToLeft = false;
};

// Index of a pixel within one 8x8 board, 0..63.
constexpr int tile_index(int lx, int ly, const Wiring& w) {
  const bool flipX = (w.first == Corner::TopRight || w.first == Corner::BottomRight);
  const bool flipY = (w.first == Corner::BottomLeft || w.first == Corner::BottomRight);
  const int fx = flipX ? (kTileW - 1 - lx) : lx;
  const int fy = flipY ? (kTileH - 1 - ly) : ly;

  const int line = (w.major == Axis::Row) ? fy : fx;  // which row/column
  int pos = (w.major == Axis::Row) ? fx : fy;         // position along it
  if (w.serpentine && (line & 1)) {
    pos = ((w.major == Axis::Row) ? kTileW : kTileH) - 1 - pos;
  }
  return line * ((w.major == Axis::Row) ? kTileW : kTileH) + pos;
}

// Index of a logical pixel in the whole 192-LED chain.
constexpr int led_index(int x, int y, const Wiring& w) {
  const int tile = x / kTileW;
  const int chainPos = w.chainRightToLeft ? (kTiles - 1 - tile) : tile;
  return chainPos * (kTileW * kTileH) + tile_index(x % kTileW, y, w);
}

constexpr bool in_bounds(int x, int y) {
  return x >= 0 && x < kWidth && y >= 0 && y < kHeight;
}

}  // namespace panel
