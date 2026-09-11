// Rolling digits: when a digit changes, the old one rolls out and the new one
// rolls in behind it, clipped to its own 5-row band.
//
// There is deliberately no clip rectangle on Framebuffer. Adding one would
// change a class every pattern uses for the benefit of one effect, so the
// clipping happens at the source instead: a glyph row outside the band is
// simply not emitted, and the row straddling the edge is emitted with reduced
// coverage.
#pragma once
#include <cstdint>

#include "panel/color.h"

namespace panel {

class Framebuffer;

constexpr float kRollSeconds = 0.18f;

struct DigitRoll {
  char from = '0';
  char to = '0';
  double t0 = 0.0;
  int8_t dir = -1;  // -1 rolls downward (a countdown), +1 upward (a clock)
  bool active = false;

  // No-op when the digit has not actually changed.
  void set(char c, double now_s, int8_t direction);
  bool running(double now_s) const;
};

// Draws the roll, clipped to rows [y, y+4].
void draw_rolling_digit(Framebuffer& fb, int x, int y, const DigitRoll& r, double now_s,
                        RGB color);

// The four digits of a time face plus the colon's phase.
struct PairFaceAnim {
  DigitRoll d[4];
  bool primed = false;
};

// Detects the changes itself and picks the roll direction from the delta.
// Draws the 18 px face centred, rows y..y+4.
void draw_pair_face_anim(Framebuffer& fb, PairFaceAnim& anim, int left, int right,
                         bool show_colon, RGB color, double now_s);

}  // namespace panel
