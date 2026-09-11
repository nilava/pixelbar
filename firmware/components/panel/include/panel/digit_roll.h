// Spinning digits: when a digit changes, its face turns over in place like the
// face of a mechanical counter wheel, showing the new digit on the back.
//
// This used to be a translation — the old glyph slid up and out while the new
// one slid in behind it. It was replaced because a translation at 24 columns
// reads as pixels moving, not as an object turning: you can see the glyph leave
// its slot. A rotation never leaves the slot. The glyph collapses to a bright
// line and opens out again, which is what the projection of a turning disc
// actually looks like, and the eye reads it as a physical wheel.
//
// There is deliberately no clip rectangle on Framebuffer. Adding one would
// change a class every pattern uses for the benefit of one effect, so the
// clipping happens at the source instead: the transform is told the band and
// simply does not emit rows outside it.
#pragma once
#include <cstdint>

#include "panel/color.h"

namespace panel {

class Framebuffer;

// One face-turn. Long enough to read as a physical movement, short enough that
// a fast encoder sweep does not queue up a visible backlog: Busy Bar spends
// about 70-100 ms per detent when the knob is spun, and this sits with it.
constexpr float kRollSeconds = 0.16f;

struct DigitRoll {
  char from = '0';
  char to = '0';
  double t0 = 0.0;
  int8_t dir = -1;  // which way the wheel turns; -1 counts down, +1 counts up
  bool active = false;

  // No-op when the digit has not actually changed.
  void set(char c, double now_s, int8_t direction);
  bool running(double now_s) const;
  // 0 at rest, else 0..1 across the turn.
  float progress(double now_s) const;
};

// Draws the turn, clipped to rows [y, y+4].
void draw_rolling_digit(Framebuffer& fb, int x, int y, const DigitRoll& r, double now_s,
                        RGB color);

// The four digits of a time face plus the colon's phase.
struct PairFaceAnim {
  DigitRoll d[4];
  bool primed = false;
};

// Detects the changes itself and picks the turn direction from the delta.
// Draws the 18 px face centred, rows y..y+4.
void draw_pair_face_anim(Framebuffer& fb, PairFaceAnim& anim, int left, int right,
                         bool show_colon, RGB color, double now_s);

}  // namespace panel
