// A 3x5 proportional font, so a word can sit beside an 8x8 icon.
//
// The panel is 24 columns. An icon takes 8 and the gutter takes 1, leaving 15
// for a label. In this font a four-letter word is exactly 15 px, which is what
// makes the status layout work without ever scrolling.
//
// Most glyphs are 3 wide. I is 1. M and W are 5, because at 3 columns there is
// only one interior column and both degenerate into something indistinguishable
// from N and A. The font is proportional anyway, so widening them is honest.
//
// Digits reuse the existing 3x5 set in font.h rather than being redrawn.
#pragma once
#include <cstdint>

#include "panel/color.h"

namespace panel {

class Framebuffer;

constexpr int kMiniH = 5;
constexpr int kMiniMaxW = 5;
constexpr int kMiniGap = 1;
constexpr int kMiniSpaceAdvance = 2;  // one blank column plus the gap

// Width of the label box beside an 8 px icon: 24 - 8 - 1.
constexpr int kMiniLabelBox = 15;

struct MiniGlyph {
  uint8_t rows[kMiniH];  // bit 4 is the leftmost column
  uint8_t w;             // ink width, 1..5
};

// Folds lowercase to uppercase. Returns nullptr for characters with no glyph.
const MiniGlyph* mini_glyph_for(char c);

int mini_char_advance(char c);          // ink width plus the gap
int mini_measure_text(const char* s);   // sum of advances
int mini_text_ink_width(const char* s); // measure minus the trailing gap
bool mini_text_fits(const char* s, int box_w = kMiniLabelBox);

int mini_draw_char(Framebuffer& fb, int x, int y, char c, RGB color);
int mini_draw_text(Framebuffer& fb, int x, int y, const char* s, RGB color);

// Centres within [x0, x0+box_w). Returns the x it started at.
int mini_draw_text_centered(Framebuffer& fb, int x0, int box_w, int y,
                            const char* s, RGB color);

// Sub-pixel, for marquees and slides. Clipped to [clip_x0, clip_x1).
void mini_draw_text_aa(Framebuffer& fb, float x, int y, const char* s, RGB color,
                       int clip_x0 = 0, int clip_x1 = 24);

}  // namespace panel
