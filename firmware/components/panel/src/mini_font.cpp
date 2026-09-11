#include "panel/mini_font.h"

#include <cmath>

#include "panel/font.h"
#include "panel/framebuffer.h"

namespace panel {
namespace {

// Left-aligned in a 5-bit field, so the same reading style as font.cpp works
// for the 5-wide glyphs too.
#define M(a, b, c, d, e) \
  static_cast<uint8_t>((a) << 4 | (b) << 3 | (c) << 2 | (d) << 1 | (e))

struct Entry {
  char ch;
  MiniGlyph g;
};

const Entry kMini[] = {
  // A has a pointed apex so it cannot be confused with R, which is the only
  // collision that would matter: CALL and FREE both depend on it.
  {'A', {{M(0,1,0,0,0), M(1,0,1,0,0), M(1,1,1,0,0), M(1,0,1,0,0), M(1,0,1,0,0)}, 3}},
  {'B', {{M(1,1,0,0,0), M(1,0,1,0,0), M(1,1,0,0,0), M(1,0,1,0,0), M(1,1,0,0,0)}, 3}},
  {'C', {{M(0,1,1,0,0), M(1,0,0,0,0), M(1,0,0,0,0), M(1,0,0,0,0), M(0,1,1,0,0)}, 3}},
  {'D', {{M(1,1,0,0,0), M(1,0,1,0,0), M(1,0,1,0,0), M(1,0,1,0,0), M(1,1,0,0,0)}, 3}},
  {'E', {{M(1,1,1,0,0), M(1,0,0,0,0), M(1,1,0,0,0), M(1,0,0,0,0), M(1,1,1,0,0)}, 3}},
  {'F', {{M(1,1,1,0,0), M(1,0,0,0,0), M(1,1,0,0,0), M(1,0,0,0,0), M(1,0,0,0,0)}, 3}},
  {'G', {{M(0,1,1,0,0), M(1,0,0,0,0), M(1,0,1,0,0), M(1,0,1,0,0), M(0,1,1,0,0)}, 3}},
  // H's crossbar sits on the middle row, N's on the second. That one row is
  // the whole difference between them at this size.
  {'H', {{M(1,0,1,0,0), M(1,0,1,0,0), M(1,1,1,0,0), M(1,0,1,0,0), M(1,0,1,0,0)}, 3}},
  {'I', {{M(1,0,0,0,0), M(1,0,0,0,0), M(1,0,0,0,0), M(1,0,0,0,0), M(1,0,0,0,0)}, 1}},
  {'J', {{M(0,0,1,0,0), M(0,0,1,0,0), M(0,0,1,0,0), M(1,0,1,0,0), M(0,1,1,0,0)}, 3}},
  {'K', {{M(1,0,1,0,0), M(1,1,0,0,0), M(1,0,0,0,0), M(1,1,0,0,0), M(1,0,1,0,0)}, 3}},
  {'L', {{M(1,0,0,0,0), M(1,0,0,0,0), M(1,0,0,0,0), M(1,0,0,0,0), M(1,1,1,0,0)}, 3}},
  {'M', {{M(1,0,0,0,1), M(1,1,0,1,1), M(1,0,1,0,1), M(1,0,0,0,1), M(1,0,0,0,1)}, 5}},
  {'N', {{M(1,0,1,0,0), M(1,1,1,0,0), M(1,0,1,0,0), M(1,0,1,0,0), M(1,0,1,0,0)}, 3}},
  {'O', {{M(1,1,1,0,0), M(1,0,1,0,0), M(1,0,1,0,0), M(1,0,1,0,0), M(1,1,1,0,0)}, 3}},
  {'P', {{M(1,1,1,0,0), M(1,0,1,0,0), M(1,1,1,0,0), M(1,0,0,0,0), M(1,0,0,0,0)}, 3}},
  {'Q', {{M(1,1,1,0,0), M(1,0,1,0,0), M(1,0,1,0,0), M(1,1,1,0,0), M(0,0,1,0,0)}, 3}},
  {'R', {{M(1,1,1,0,0), M(1,0,1,0,0), M(1,1,1,0,0), M(1,0,1,0,0), M(1,0,1,0,0)}, 3}},
  {'S', {{M(1,1,1,0,0), M(1,0,0,0,0), M(1,1,1,0,0), M(0,0,1,0,0), M(1,1,1,0,0)}, 3}},
  {'T', {{M(1,1,1,0,0), M(0,1,0,0,0), M(0,1,0,0,0), M(0,1,0,0,0), M(0,1,0,0,0)}, 3}},
  {'U', {{M(1,0,1,0,0), M(1,0,1,0,0), M(1,0,1,0,0), M(1,0,1,0,0), M(1,1,1,0,0)}, 3}},
  {'V', {{M(1,0,1,0,0), M(1,0,1,0,0), M(1,0,1,0,0), M(1,0,1,0,0), M(0,1,0,0,0)}, 3}},
  {'W', {{M(1,0,0,0,1), M(1,0,0,0,1), M(1,0,1,0,1), M(1,1,0,1,1), M(1,0,0,0,1)}, 5}},
  {'X', {{M(1,0,1,0,0), M(1,0,1,0,0), M(0,1,0,0,0), M(1,0,1,0,0), M(1,0,1,0,0)}, 3}},
  {'Y', {{M(1,0,1,0,0), M(1,0,1,0,0), M(0,1,0,0,0), M(0,1,0,0,0), M(0,1,0,0,0)}, 3}},
  {'Z', {{M(1,1,1,0,0), M(0,0,1,0,0), M(0,1,0,0,0), M(1,0,0,0,0), M(1,1,1,0,0)}, 3}},
  {':', {{M(0,0,0,0,0), M(1,0,0,0,0), M(0,0,0,0,0), M(1,0,0,0,0), M(0,0,0,0,0)}, 1}},
  {'.', {{M(0,0,0,0,0), M(0,0,0,0,0), M(0,0,0,0,0), M(0,0,0,0,0), M(1,0,0,0,0)}, 1}},
  {'!', {{M(1,0,0,0,0), M(1,0,0,0,0), M(1,0,0,0,0), M(0,0,0,0,0), M(1,0,0,0,0)}, 1}},
  {'-', {{M(0,0,0,0,0), M(0,0,0,0,0), M(1,1,1,0,0), M(0,0,0,0,0), M(0,0,0,0,0)}, 3}},
  {'/', {{M(0,0,1,0,0), M(0,0,1,0,0), M(0,1,0,0,0), M(1,0,0,0,0), M(1,0,0,0,0)}, 3}},
  {'%', {{M(1,0,1,0,0), M(0,0,1,0,0), M(0,1,0,0,0), M(1,0,0,0,0), M(1,0,1,0,0)}, 3}},
  {'?', {{M(1,1,1,0,0), M(0,0,1,0,0), M(0,1,1,0,0), M(0,0,0,0,0), M(0,1,0,0,0)}, 3}},
  {'\'',{{M(1,0,0,0,0), M(1,0,0,0,0), M(0,0,0,0,0), M(0,0,0,0,0), M(0,0,0,0,0)}, 1}},
  {'+', {{M(0,0,0,0,0), M(0,1,0,0,0), M(1,1,1,0,0), M(0,1,0,0,0), M(0,0,0,0,0)}, 3}},
};
#undef M

constexpr int kMiniCount = sizeof(kMini) / sizeof(kMini[0]);

char fold(char c) {
  return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

// Digits are the 3x5 set already in font.h, promoted into a MiniGlyph so the
// two fonts share one code path and can never drift apart.
bool digit_glyph(char c, MiniGlyph* out) {
  const uint8_t* d = tiny_digit(c);
  if (!d) return false;
  for (int r = 0; r < kMiniH; ++r) {
    // tiny_digit packs 3 bits with bit 2 leftmost; shift into the 5-bit field.
    out->rows[r] = static_cast<uint8_t>((d[r] & 0x7) << 2);
  }
  out->w = 3;
  return true;
}

}  // namespace

const MiniGlyph* mini_glyph_for(char c) {
  static MiniGlyph digit;  // single-threaded drawing; refilled per lookup
  const char f = fold(c);
  if (digit_glyph(f, &digit)) return &digit;
  for (int i = 0; i < kMiniCount; ++i) {
    if (kMini[i].ch == f) return &kMini[i].g;
  }
  return nullptr;
}

int mini_char_advance(char c) {
  const MiniGlyph* g = mini_glyph_for(c);
  return g ? g->w + kMiniGap : kMiniSpaceAdvance;
}

int mini_measure_text(const char* s) {
  int total = 0;
  for (; s && *s; ++s) total += mini_char_advance(*s);
  return total;
}

int mini_text_ink_width(const char* s) {
  const int w = mini_measure_text(s);
  return w > 0 ? w - kMiniGap : 0;
}

bool mini_text_fits(const char* s, int box_w) {
  return mini_text_ink_width(s) <= box_w;
}

int mini_draw_char(Framebuffer& fb, int x, int y, char c, RGB color) {
  const MiniGlyph* g = mini_glyph_for(c);
  if (!g) return kMiniSpaceAdvance;
  for (int row = 0; row < kMiniH; ++row) {
    for (int col = 0; col < g->w; ++col) {
      if (g->rows[row] & (1 << (kMiniMaxW - 1 - col))) fb.set(x + col, y + row, color);
    }
  }
  return g->w + kMiniGap;
}

int mini_draw_text(Framebuffer& fb, int x, int y, const char* s, RGB color) {
  int cx = x;
  for (; s && *s; ++s) cx += mini_draw_char(fb, cx, y, *s, color);
  return cx - x;
}

int mini_draw_text_centered(Framebuffer& fb, int x0, int box_w, int y,
                            const char* s, RGB color) {
  const int x = x0 + (box_w - mini_text_ink_width(s)) / 2;
  mini_draw_text(fb, x, y, s, color);
  return x;
}

void mini_draw_text_aa(Framebuffer& fb, float x, int y, const char* s, RGB color,
                       int clip_x0, int clip_x1) {
  float cx = x;
  for (; s && *s; ++s) {
    const MiniGlyph* g = mini_glyph_for(*s);
    if (!g) {
      cx += kMiniSpaceAdvance;
      continue;
    }
    for (int row = 0; row < kMiniH; ++row) {
      for (int col = 0; col < g->w; ++col) {
        if (!(g->rows[row] & (1 << (kMiniMaxW - 1 - col)))) continue;
        const float px = cx + col;
        // Framebuffer::set clips at the panel edge, but a marquee has to be
        // masked by the icon gutter too, so clip explicitly here.
        if (px + 1.0f <= clip_x0 || px >= clip_x1) continue;
        fb.set_aa(px, y + row, color, 1.0f, Blend::Add);
      }
    }
    cx += g->w + kMiniGap;
  }
}

}  // namespace panel
