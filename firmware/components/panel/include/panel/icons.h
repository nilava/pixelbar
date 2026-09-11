// 8x8 icons, one bit per pixel, coloured when drawn.
//
// At 8 rows the panel cannot show a word and a picture in the 5x7 font, so the
// icon carries the meaning and the 3x5 mini font carries the label. An icon is
// 8 columns, the gutter is 1, the label box is 15: exactly 24.
#pragma once
#include <cstdint>

#include "panel/color.h"

namespace panel {

class Framebuffer;

constexpr int kIconW = 8;
constexpr int kIconH = 8;

struct Icon {
  uint8_t rows[kIconH];  // bit 7 is the leftmost column
};

// A looping sequence of frames.
struct AnimIcon {
  const Icon* frames;
  uint8_t count;
  float fps;
  bool ping_pong;
};

void draw_icon(Framebuffer& fb, int x, int y, const Icon& ic, RGB color);
void draw_icon_aa(Framebuffer& fb, float x, float y, const Icon& ic, RGB color);
int icon_lit_count(const Icon& ic);

const Icon& anim_frame(const AnimIcon& a, double t_s);
void draw_anim_icon(Framebuffer& fb, int x, int y, const AnimIcon& a, double t_s, RGB color);

// A shell plus a partial fill from the bottom, with a sub-pixel boundary row.
// Used for the hourglass and the signal strength.
void draw_icon_masked(Framebuffer& fb, int x, int y, const Icon& shell, const Icon& fill,
                      float fraction, RGB shell_c, RGB fill_c);

// An icon drawn as an ordered set of points, so it can draw itself on.
struct StrokeIcon {
  const uint8_t* pts;  // each byte is (x << 4) | y
  uint8_t n;
};
void draw_stroke_icon(Framebuffer& fb, int x, int y, const StrokeIcon& s, float progress,
                      RGB body, RGB tip);

// ------------------------------------------------------------- the set
extern const Icon kIconFree;       // an open ring
extern const Icon kIconBusy;       // the same ring, filled: the dissolve reads as solidifying
extern const Icon kIconDnd;        // ring with a slash
extern const AnimIcon kAnimCall;   // a handset with ripples going out
extern const Icon kIconMoon;
extern const Icon kIconSunCore;
extern const Icon kIconSunRays;
extern const Icon kIconHourglass;      // the shell
extern const Icon kIconHourglassTop;   // upper chamber, the sand that is left
extern const Icon kIconHourglassBottom;// lower chamber, the sand that has fallen
extern const AnimIcon kAnimWifi;   // arcs filling
extern const StrokeIcon kStrokeCheck;

// The hourglass drawn for a given remaining fraction, with a falling grain.
void draw_hourglass(Framebuffer& fb, int x, int y, float remaining, double t_s,
                    RGB shell, RGB sand);

// The one multi-coloured icon, so it gets its own function.

}  // namespace panel
