// Bitmap fonts.
//
// kFont5x7 is the main proportional font: glyphs are stored 5 wide and 7 tall,
// then trimmed to their ink when drawn, so 'I' and ':' take less room than 'W'.
// It is uppercase only; lowercase input is folded to uppercase.
//
// kFont3x5 is a digits-only face used by the clock layout, where four digits
// plus a colon have to fit across 24 pixels.
#pragma once
#include <cstdint>

#include "panel/color.h"

namespace panel {

class Framebuffer;

constexpr int kGlyphH = 7;
constexpr int kGlyphW = 5;
constexpr int kSpaceAdvance = 3;  // blank columns + the inter-character gap
constexpr int kCharGap = 1;

struct Glyph {
  // One byte per row, bit 4 is the leftmost column.
  uint8_t rows[kGlyphH];
};

// Returns nullptr for characters with no glyph.
const Glyph* glyph_for(char c);

// Ink extent of a glyph, 0 if blank.
int glyph_width(const Glyph& g);

// Advance width of one character including the gap that follows it.
int char_advance(char c);

// Total advance width of a string.
int measure_text(const char* s);

// Draws with the glyph's top row at y. Returns the advance width.
int draw_char(Framebuffer& fb, int x, int y, char c, RGB color);

// Returns the advance width drawn.
int draw_text(Framebuffer& fb, int x, int y, const char* s, RGB color);

// Width of the ink alone, without the trailing inter-character gap.
int text_ink_width(const char* s);

// True if the string can be shown whole, without scrolling.
bool text_fits(const char* s);

// Draws horizontally centred. Returns the x it started at.
int draw_text_centered(Framebuffer& fb, int y, const char* s, RGB color);

// 3x5 digits, '0'..'9'. Returns nullptr otherwise. 5 rows, bit 2 leftmost.
const uint8_t* tiny_digit(char c);
constexpr int kTinyW = 3;
constexpr int kTinyH = 5;

void draw_tiny_digit(Framebuffer& fb, int x, int y, char c, RGB color);

}  // namespace panel
