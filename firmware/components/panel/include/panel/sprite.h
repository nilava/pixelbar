// Drawing a 1-bit bitmap through a transform, about its own centre.
//
// This is the primitive the UI motion is built on. The distinction that
// matters: a slide moves a glyph from one place to another, and at 24 columns
// you can count the columns going past. A transform leaves the glyph where it
// is and changes its *shape* — squashing it flat and letting it open out again
// reads as a physical disc turning, because that is exactly the orthographic
// projection of one.
//
// Everything here is coverage-weighted. A vertical scale of 0.37 really is a
// glyph 37% as tall, not one rounded to two rows, which is what stops the
// rotation from strobing through a handful of discrete shapes.
#pragma once
#include <cstdint>

#include "panel/color.h"
#include "panel/framebuffer.h"

namespace panel {

struct Icon;
struct MiniGlyph;

// A 1-bit source bitmap: h rows of packed bits, column 0 at bit `msb`.
//
// The two bitmap types in the project pack their bits differently (an Icon is
// 8 wide from bit 7, a MiniGlyph is up to 5 wide from bit 4), so this carries
// the bit position rather than assuming one.
struct Sprite {
  const uint8_t* rows = nullptr;
  uint8_t w = 0;
  uint8_t h = 0;
  uint8_t msb = 7;

  bool on(int x, int y) const {
    if (x < 0 || y < 0 || x >= w || y >= h) return false;
    return (rows[y] >> (msb - x)) & 1u;
  }
};

Sprite sprite_of(const Icon& ic);
Sprite sprite_of(const MiniGlyph& g);

// How a transform treats the ink when it shrinks something.
//
// Normalised keeps the peak brightness constant, so a glyph squashed to a
// third of its height is the same brightness over fewer rows. Conserved keeps
// the total ink constant, so it gets brighter as it collapses.
//
// Normalised is right for a turning face, because a real surface emits per
// unit area and presenting less area to you does not make it hotter. Conserved
// is right for something contracting to a point, like a spark.
enum class Ink : uint8_t { Normalised, Conserved };

// Draw src centred on (cx, cy), scaled by (sx, sy) about that centre.
//
// cx/cy are in panel coordinates and address the *centre* of the sprite, so a
// 5-row glyph whose top row is y sits at cy = y + 2.5. Negative scales mirror.
// Rows outside [clip_y0, clip_y1) are not written, which is how a digit stays
// inside its own band while it turns.
void draw_sprite_scaled(Framebuffer& fb, const Sprite& s, float cx, float cy,
                        float sx, float sy, RGB color, float bright = 1.0f,
                        int clip_y0 = 0, int clip_y1 = kHeight,
                        Ink ink = Ink::Normalised, Blend b = Blend::Add);

// Draw src rotated about its centre by `turns` (1.0 is a full revolution).
//
// Inverse-mapped and bilinearly sampled: at 8x8 no rotation can be crisp, and
// trying to keep it crisp is what makes a rotating icon strobe. Letting it go
// soft between the cardinal angles is what sells the movement.
void draw_sprite_rotated(Framebuffer& fb, const Sprite& s, float cx, float cy,
                         float turns, RGB color, float bright = 1.0f,
                         float scale = 1.0f, Blend b = Blend::Add);

// ------------------------------------------------------------- the spin
//
// One face of a disc turning to show another. theta runs 0..0.5 turns: the
// first half collapses `from` to an edge, the second opens `to` out of it.

// How wide the bright rim is, as a fraction of the half-turn either side of
// edge-on. The rim is what makes it read as a solid object catching the light
// rather than a glyph being squashed by an invisible hand.
constexpr float kSpinRimWidth = 0.13f;

// Brightness of a face presenting cos(theta) of its area to the viewer.
// Never reaches zero: a face that dims to nothing as it turns just vanishes,
// and the eye reads a vanish as a fade, not a rotation.
float spin_face_bright(float turns);

// Vertical scale of a face at this angle. |cos| of the turn.
float spin_face_scale(float turns);

// Draws one spinning face, picking `from` before the halfway point and `to`
// after it, and flaring the rim as it passes through edge-on.
void draw_spin(Framebuffer& fb, const Sprite& from, const Sprite& to, float cx,
               float cy, float turns, RGB color, int clip_y0 = 0,
               int clip_y1 = kHeight);

// ------------------------------------------------------------- the pop
//
// An element leaving by shrinking into itself while the next one grows out of
// the same spot. u runs 0..1 across the whole exchange.

constexpr float kPopSwapPoint = 0.45f;  // where the old one is fully gone
constexpr float kPopSeconds = 0.22f;

// Scale of the outgoing element, 1 -> 0 over [0, kPopSwapPoint].
float pop_out_scale(float u);
// Scale of the incoming element, 0 -> 1 with a small overshoot after it.
float pop_in_scale(float u);

// Remembers when a value last changed, so the element showing it can turn over
// where it stands instead of the whole screen cross-fading.
//
// The distinction is the point of this file. A screen-level transition says
// "you are somewhere else now". An element swap says "this one thing changed",
// which is almost always what actually happened, and it keeps the rest of the
// layout still while it happens.
struct SwapState {
  double t0 = 0.0;
  bool primed = false;

  void trigger(double now_s) { t0 = now_s; }
  bool running(double now_s, float seconds = kPopSeconds) const {
    return primed && (now_s - t0) < seconds;
  }
  // 1.0 when nothing is happening, else 0..1 across the exchange.
  float u(double now_s, float seconds = kPopSeconds) const;
};

// An icon drawn about its own centre at an arbitrary scale.
void draw_icon_scaled(Framebuffer& fb, int x, int y, const Icon& ic, RGB color,
                      float sx, float sy, float bright = 1.0f);

// A mini-font label squashed vertically about its own middle, letters staying
// where they are. Busy Bar's menu labels do exactly this while their icon
// scales in both axes, and the asymmetry is what keeps the word readable for
// longer than the picture.
void mini_draw_text_squashed(Framebuffer& fb, int x0, int box_w, int y,
                             const char* s, RGB color, float sy,
                             float bright = 1.0f);

}  // namespace panel
