#include "panel/font.h"

#include "panel/framebuffer.h"

namespace panel {
namespace {

// 0b11111 is the leftmost-aligned 5-bit row. Written as binary so the shapes
// are readable in the source.
#define R(a, b, c, d, e) \
  static_cast<uint8_t>((a) << 4 | (b) << 3 | (c) << 2 | (d) << 1 | (e))

struct Entry {
  char ch;
  Glyph g;
};

const Entry kFont[] = {
  {' ', {{R(0,0,0,0,0),R(0,0,0,0,0),R(0,0,0,0,0),R(0,0,0,0,0),R(0,0,0,0,0),R(0,0,0,0,0),R(0,0,0,0,0)}}},
  {'A', {{R(0,1,1,1,0),R(1,0,0,0,1),R(1,0,0,0,1),R(1,1,1,1,1),R(1,0,0,0,1),R(1,0,0,0,1),R(1,0,0,0,1)}}},
  {'B', {{R(1,1,1,1,0),R(1,0,0,0,1),R(1,0,0,0,1),R(1,1,1,1,0),R(1,0,0,0,1),R(1,0,0,0,1),R(1,1,1,1,0)}}},
  {'C', {{R(0,1,1,1,0),R(1,0,0,0,1),R(1,0,0,0,0),R(1,0,0,0,0),R(1,0,0,0,0),R(1,0,0,0,1),R(0,1,1,1,0)}}},
  {'D', {{R(1,1,1,0,0),R(1,0,0,1,0),R(1,0,0,0,1),R(1,0,0,0,1),R(1,0,0,0,1),R(1,0,0,1,0),R(1,1,1,0,0)}}},
  {'E', {{R(1,1,1,1,1),R(1,0,0,0,0),R(1,0,0,0,0),R(1,1,1,1,0),R(1,0,0,0,0),R(1,0,0,0,0),R(1,1,1,1,1)}}},
  {'F', {{R(1,1,1,1,1),R(1,0,0,0,0),R(1,0,0,0,0),R(1,1,1,1,0),R(1,0,0,0,0),R(1,0,0,0,0),R(1,0,0,0,0)}}},
  {'G', {{R(0,1,1,1,0),R(1,0,0,0,1),R(1,0,0,0,0),R(1,0,1,1,1),R(1,0,0,0,1),R(1,0,0,0,1),R(0,1,1,1,1)}}},
  {'H', {{R(1,0,0,0,1),R(1,0,0,0,1),R(1,0,0,0,1),R(1,1,1,1,1),R(1,0,0,0,1),R(1,0,0,0,1),R(1,0,0,0,1)}}},
  {'I', {{R(0,1,1,1,0),R(0,0,1,0,0),R(0,0,1,0,0),R(0,0,1,0,0),R(0,0,1,0,0),R(0,0,1,0,0),R(0,1,1,1,0)}}},
  {'J', {{R(0,0,1,1,1),R(0,0,0,1,0),R(0,0,0,1,0),R(0,0,0,1,0),R(0,0,0,1,0),R(1,0,0,1,0),R(0,1,1,0,0)}}},
  {'K', {{R(1,0,0,0,1),R(1,0,0,1,0),R(1,0,1,0,0),R(1,1,0,0,0),R(1,0,1,0,0),R(1,0,0,1,0),R(1,0,0,0,1)}}},
  {'L', {{R(1,0,0,0,0),R(1,0,0,0,0),R(1,0,0,0,0),R(1,0,0,0,0),R(1,0,0,0,0),R(1,0,0,0,0),R(1,1,1,1,1)}}},
  {'M', {{R(1,0,0,0,1),R(1,1,0,1,1),R(1,0,1,0,1),R(1,0,1,0,1),R(1,0,0,0,1),R(1,0,0,0,1),R(1,0,0,0,1)}}},
  {'N', {{R(1,0,0,0,1),R(1,0,0,0,1),R(1,1,0,0,1),R(1,0,1,0,1),R(1,0,0,1,1),R(1,0,0,0,1),R(1,0,0,0,1)}}},
  {'O', {{R(0,1,1,1,0),R(1,0,0,0,1),R(1,0,0,0,1),R(1,0,0,0,1),R(1,0,0,0,1),R(1,0,0,0,1),R(0,1,1,1,0)}}},
  {'P', {{R(1,1,1,1,0),R(1,0,0,0,1),R(1,0,0,0,1),R(1,1,1,1,0),R(1,0,0,0,0),R(1,0,0,0,0),R(1,0,0,0,0)}}},
  {'Q', {{R(0,1,1,1,0),R(1,0,0,0,1),R(1,0,0,0,1),R(1,0,0,0,1),R(1,0,1,0,1),R(1,0,0,1,0),R(0,1,1,0,1)}}},
  {'R', {{R(1,1,1,1,0),R(1,0,0,0,1),R(1,0,0,0,1),R(1,1,1,1,0),R(1,0,1,0,0),R(1,0,0,1,0),R(1,0,0,0,1)}}},
  {'S', {{R(0,1,1,1,1),R(1,0,0,0,0),R(1,0,0,0,0),R(0,1,1,1,0),R(0,0,0,0,1),R(0,0,0,0,1),R(1,1,1,1,0)}}},
  {'T', {{R(1,1,1,1,1),R(0,0,1,0,0),R(0,0,1,0,0),R(0,0,1,0,0),R(0,0,1,0,0),R(0,0,1,0,0),R(0,0,1,0,0)}}},
  {'U', {{R(1,0,0,0,1),R(1,0,0,0,1),R(1,0,0,0,1),R(1,0,0,0,1),R(1,0,0,0,1),R(1,0,0,0,1),R(0,1,1,1,0)}}},
  {'V', {{R(1,0,0,0,1),R(1,0,0,0,1),R(1,0,0,0,1),R(1,0,0,0,1),R(1,0,0,0,1),R(0,1,0,1,0),R(0,0,1,0,0)}}},
  {'W', {{R(1,0,0,0,1),R(1,0,0,0,1),R(1,0,0,0,1),R(1,0,1,0,1),R(1,0,1,0,1),R(1,0,1,0,1),R(0,1,0,1,0)}}},
  {'X', {{R(1,0,0,0,1),R(1,0,0,0,1),R(0,1,0,1,0),R(0,0,1,0,0),R(0,1,0,1,0),R(1,0,0,0,1),R(1,0,0,0,1)}}},
  {'Y', {{R(1,0,0,0,1),R(1,0,0,0,1),R(1,0,0,0,1),R(0,1,0,1,0),R(0,0,1,0,0),R(0,0,1,0,0),R(0,0,1,0,0)}}},
  {'Z', {{R(1,1,1,1,1),R(0,0,0,0,1),R(0,0,0,1,0),R(0,0,1,0,0),R(0,1,0,0,0),R(1,0,0,0,0),R(1,1,1,1,1)}}},
  {'0', {{R(0,1,1,1,0),R(1,0,0,0,1),R(1,0,0,1,1),R(1,0,1,0,1),R(1,1,0,0,1),R(1,0,0,0,1),R(0,1,1,1,0)}}},
  {'1', {{R(0,0,1,0,0),R(0,1,1,0,0),R(0,0,1,0,0),R(0,0,1,0,0),R(0,0,1,0,0),R(0,0,1,0,0),R(0,1,1,1,0)}}},
  {'2', {{R(0,1,1,1,0),R(1,0,0,0,1),R(0,0,0,0,1),R(0,0,0,1,0),R(0,0,1,0,0),R(0,1,0,0,0),R(1,1,1,1,1)}}},
  {'3', {{R(1,1,1,1,1),R(0,0,0,1,0),R(0,0,1,0,0),R(0,0,0,1,0),R(0,0,0,0,1),R(1,0,0,0,1),R(0,1,1,1,0)}}},
  {'4', {{R(0,0,0,1,0),R(0,0,1,1,0),R(0,1,0,1,0),R(1,0,0,1,0),R(1,1,1,1,1),R(0,0,0,1,0),R(0,0,0,1,0)}}},
  {'5', {{R(1,1,1,1,1),R(1,0,0,0,0),R(1,1,1,1,0),R(0,0,0,0,1),R(0,0,0,0,1),R(1,0,0,0,1),R(0,1,1,1,0)}}},
  {'6', {{R(0,0,1,1,0),R(0,1,0,0,0),R(1,0,0,0,0),R(1,1,1,1,0),R(1,0,0,0,1),R(1,0,0,0,1),R(0,1,1,1,0)}}},
  {'7', {{R(1,1,1,1,1),R(0,0,0,0,1),R(0,0,0,1,0),R(0,0,1,0,0),R(0,1,0,0,0),R(0,1,0,0,0),R(0,1,0,0,0)}}},
  {'8', {{R(0,1,1,1,0),R(1,0,0,0,1),R(1,0,0,0,1),R(0,1,1,1,0),R(1,0,0,0,1),R(1,0,0,0,1),R(0,1,1,1,0)}}},
  {'9', {{R(0,1,1,1,0),R(1,0,0,0,1),R(1,0,0,0,1),R(0,1,1,1,1),R(0,0,0,0,1),R(0,0,0,1,0),R(0,1,1,0,0)}}},
  {':', {{R(0,0,0,0,0),R(0,0,1,0,0),R(0,0,1,0,0),R(0,0,0,0,0),R(0,0,1,0,0),R(0,0,1,0,0),R(0,0,0,0,0)}}},
  {';', {{R(0,0,0,0,0),R(0,0,1,0,0),R(0,0,0,0,0),R(0,0,0,0,0),R(0,0,1,0,0),R(0,0,1,0,0),R(0,1,0,0,0)}}},
  {'.', {{R(0,0,0,0,0),R(0,0,0,0,0),R(0,0,0,0,0),R(0,0,0,0,0),R(0,0,0,0,0),R(0,1,1,0,0),R(0,1,1,0,0)}}},
  {',', {{R(0,0,0,0,0),R(0,0,0,0,0),R(0,0,0,0,0),R(0,0,0,0,0),R(0,1,1,0,0),R(0,0,1,0,0),R(0,1,0,0,0)}}},
  {'!', {{R(0,0,1,0,0),R(0,0,1,0,0),R(0,0,1,0,0),R(0,0,1,0,0),R(0,0,1,0,0),R(0,0,0,0,0),R(0,0,1,0,0)}}},
  {'?', {{R(0,1,1,1,0),R(1,0,0,0,1),R(0,0,0,0,1),R(0,0,0,1,0),R(0,0,1,0,0),R(0,0,0,0,0),R(0,0,1,0,0)}}},
  {'-', {{R(0,0,0,0,0),R(0,0,0,0,0),R(0,0,0,0,0),R(1,1,1,1,1),R(0,0,0,0,0),R(0,0,0,0,0),R(0,0,0,0,0)}}},
  {'_', {{R(0,0,0,0,0),R(0,0,0,0,0),R(0,0,0,0,0),R(0,0,0,0,0),R(0,0,0,0,0),R(0,0,0,0,0),R(1,1,1,1,1)}}},
  {'+', {{R(0,0,0,0,0),R(0,0,1,0,0),R(0,0,1,0,0),R(1,1,1,1,1),R(0,0,1,0,0),R(0,0,1,0,0),R(0,0,0,0,0)}}},
  {'=', {{R(0,0,0,0,0),R(0,0,0,0,0),R(1,1,1,1,1),R(0,0,0,0,0),R(1,1,1,1,1),R(0,0,0,0,0),R(0,0,0,0,0)}}},
  {'/', {{R(0,0,0,0,1),R(0,0,0,1,0),R(0,0,0,1,0),R(0,0,1,0,0),R(0,1,0,0,0),R(0,1,0,0,0),R(1,0,0,0,0)}}},
  {'*', {{R(0,0,0,0,0),R(1,0,1,0,1),R(0,1,1,1,0),R(1,1,1,1,1),R(0,1,1,1,0),R(1,0,1,0,1),R(0,0,0,0,0)}}},
  {'#', {{R(0,1,0,1,0),R(0,1,0,1,0),R(1,1,1,1,1),R(0,1,0,1,0),R(1,1,1,1,1),R(0,1,0,1,0),R(0,1,0,1,0)}}},
  {'%', {{R(1,1,0,0,1),R(1,1,0,1,0),R(0,0,0,1,0),R(0,0,1,0,0),R(0,1,0,0,0),R(0,1,0,1,1),R(1,0,0,1,1)}}},
  {'@', {{R(0,1,1,1,0),R(1,0,0,0,1),R(1,0,1,1,1),R(1,0,1,0,1),R(1,0,1,1,1),R(1,0,0,0,0),R(0,1,1,1,0)}}},
  {'(', {{R(0,0,0,1,0),R(0,0,1,0,0),R(0,1,0,0,0),R(0,1,0,0,0),R(0,1,0,0,0),R(0,0,1,0,0),R(0,0,0,1,0)}}},
  {')', {{R(0,1,0,0,0),R(0,0,1,0,0),R(0,0,0,1,0),R(0,0,0,1,0),R(0,0,0,1,0),R(0,0,1,0,0),R(0,1,0,0,0)}}},
  {'<', {{R(0,0,0,1,0),R(0,0,1,0,0),R(0,1,0,0,0),R(1,0,0,0,0),R(0,1,0,0,0),R(0,0,1,0,0),R(0,0,0,1,0)}}},
  {'>', {{R(0,1,0,0,0),R(0,0,1,0,0),R(0,0,0,1,0),R(0,0,0,0,1),R(0,0,0,1,0),R(0,0,1,0,0),R(0,1,0,0,0)}}},
  {'\'',{{R(0,0,1,0,0),R(0,0,1,0,0),R(0,1,0,0,0),R(0,0,0,0,0),R(0,0,0,0,0),R(0,0,0,0,0),R(0,0,0,0,0)}}},
  {'"', {{R(0,1,0,1,0),R(0,1,0,1,0),R(0,0,0,0,0),R(0,0,0,0,0),R(0,0,0,0,0),R(0,0,0,0,0),R(0,0,0,0,0)}}},
};
#undef R

constexpr int kFontCount = sizeof(kFont) / sizeof(kFont[0]);

// 3x5 digits: 5 rows, bit 2 is the leftmost column.
#define T(a, b, c) static_cast<uint8_t>((a) << 2 | (b) << 1 | (c))
const uint8_t kTiny[10][kTinyH] = {
  {T(1,1,1),T(1,0,1),T(1,0,1),T(1,0,1),T(1,1,1)},
  {T(0,1,0),T(1,1,0),T(0,1,0),T(0,1,0),T(1,1,1)},
  {T(1,1,1),T(0,0,1),T(1,1,1),T(1,0,0),T(1,1,1)},
  {T(1,1,1),T(0,0,1),T(1,1,1),T(0,0,1),T(1,1,1)},
  {T(1,0,1),T(1,0,1),T(1,1,1),T(0,0,1),T(0,0,1)},
  {T(1,1,1),T(1,0,0),T(1,1,1),T(0,0,1),T(1,1,1)},
  {T(1,1,1),T(1,0,0),T(1,1,1),T(1,0,1),T(1,1,1)},
  {T(1,1,1),T(0,0,1),T(0,0,1),T(0,0,1),T(0,0,1)},
  {T(1,1,1),T(1,0,1),T(1,1,1),T(1,0,1),T(1,1,1)},
  {T(1,1,1),T(1,0,1),T(1,1,1),T(0,0,1),T(1,1,1)},
};
#undef T

char fold(char c) {
  return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

// Leftmost ink column, or -1 when the glyph is blank.
int left_edge(const Glyph& g) {
  for (int col = 0; col < kGlyphW; ++col) {
    for (int row = 0; row < kGlyphH; ++row) {
      if (g.rows[row] & (1 << (kGlyphW - 1 - col))) return col;
    }
  }
  return -1;
}

}  // namespace

const Glyph* glyph_for(char c) {
  const char f = fold(c);
  for (int i = 0; i < kFontCount; ++i) {
    if (kFont[i].ch == f) return &kFont[i].g;
  }
  return nullptr;
}

int glyph_width(const Glyph& g) {
  const int left = left_edge(g);
  if (left < 0) return 0;
  int right = left;
  for (int col = kGlyphW - 1; col >= left; --col) {
    for (int row = 0; row < kGlyphH; ++row) {
      if (g.rows[row] & (1 << (kGlyphW - 1 - col))) {
        right = col;
        col = -1;  // break the outer loop
        break;
      }
    }
  }
  return right - left + 1;
}

int char_advance(char c) {
  const Glyph* g = glyph_for(c);
  if (!g) return kSpaceAdvance;
  const int w = glyph_width(*g);
  return (w == 0) ? kSpaceAdvance : w + kCharGap;
}

int measure_text(const char* s) {
  int total = 0;
  for (; s && *s; ++s) total += char_advance(*s);
  return total;
}

int draw_char(Framebuffer& fb, int x, int y, char c, RGB color) {
  const Glyph* g = glyph_for(c);
  if (!g) return kSpaceAdvance;
  const int left = left_edge(*g);
  if (left < 0) return kSpaceAdvance;
  const int w = glyph_width(*g);
  for (int row = 0; row < kGlyphH; ++row) {
    for (int col = left; col < left + w; ++col) {
      if (g->rows[row] & (1 << (kGlyphW - 1 - col))) {
        fb.set(x + col - left, y + row, color);
      }
    }
  }
  return w + kCharGap;
}

int draw_text(Framebuffer& fb, int x, int y, const char* s, RGB color) {
  int cx = x;
  for (; s && *s; ++s) cx += draw_char(fb, cx, y, *s, color);
  return cx - x;
}

int text_ink_width(const char* s) {
  const int w = measure_text(s);
  return w > 0 ? w - kCharGap : 0;  // the last character's gap is not ink
}

bool text_fits(const char* s) { return text_ink_width(s) <= kWidth; }

int draw_text_centered(Framebuffer& fb, int y, const char* s, RGB color) {
  const int x = (kWidth - text_ink_width(s)) / 2;
  panel::draw_text(fb, x, y, s, color);
  return x;
}

float draw_char_aa(Framebuffer& fb, float x, int y, char c, RGB color) {
  const Glyph* g = glyph_for(c);
  if (!g) return kSpaceAdvance;
  const int left = left_edge(*g);
  if (left < 0) return kSpaceAdvance;
  const int w = glyph_width(*g);
  for (int row = 0; row < kGlyphH; ++row) {
    for (int col = left; col < left + w; ++col) {
      if (g->rows[row] & (1 << (kGlyphW - 1 - col))) {
        // Add, not Over: neighbouring ink columns write complementary weights
        // into the shared LED and must sum, or a solid stroke dims as it moves.
        fb.set_aa(x + (col - left), y + row, color, 1.0f, Blend::Add);
      }
    }
  }
  return static_cast<float>(w + kCharGap);
}

float draw_text_aa(Framebuffer& fb, float x, int y, const char* s, RGB color) {
  float cx = x;
  for (; s && *s; ++s) cx += draw_char_aa(fb, cx, y, *s, color);
  return cx - x;
}

void draw_tiny_digit_aa(Framebuffer& fb, float x, int y, char c, RGB color) {
  const uint8_t* d = tiny_digit(c);
  if (!d) return;
  for (int row = 0; row < kTinyH; ++row) {
    for (int col = 0; col < kTinyW; ++col) {
      if (d[row] & (1 << (kTinyW - 1 - col))) {
        fb.set_aa(x + col, y + row, color, 1.0f, Blend::Add);
      }
    }
  }
}

const uint8_t* tiny_digit(char c) {
  if (c < '0' || c > '9') return nullptr;
  return kTiny[c - '0'];
}

void draw_tiny_digit(Framebuffer& fb, int x, int y, char c, RGB color) {
  const uint8_t* d = tiny_digit(c);
  if (!d) return;
  for (int row = 0; row < kTinyH; ++row) {
    for (int col = 0; col < kTinyW; ++col) {
      if (d[row] & (1 << (kTinyW - 1 - col))) fb.set(x + col, y + row, color);
    }
  }
}

}  // namespace panel
