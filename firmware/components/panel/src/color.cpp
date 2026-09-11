#include "panel/color.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace panel {

RGB hsv(float h, float s, float v) {
  h = h - std::floor(h);  // wrap into 0..1
  if (s <= 0.0f) {
    const uint8_t g = static_cast<uint8_t>(v * 255.0f + 0.5f);
    return RGB(g, g, g);
  }
  const float hf = h * 6.0f;
  const int i = static_cast<int>(hf) % 6;
  const float f = hf - std::floor(hf);
  const float p = v * (1.0f - s);
  const float q = v * (1.0f - f * s);
  const float t = v * (1.0f - (1.0f - f) * s);
  float r = 0, g = 0, b = 0;
  switch (i) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
  }
  auto to8 = [](float x) {
    if (x < 0) x = 0;
    if (x > 1) x = 1;
    return static_cast<uint8_t>(x * 255.0f + 0.5f);
  };
  return RGB(to8(r), to8(g), to8(b));
}

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool parse_hex_color(const char* s, RGB* out) {
  if (!s || !out) return false;
  if (*s == '#') ++s;
  if (std::strlen(s) < 6) return false;
  int v[6];
  for (int i = 0; i < 6; ++i) {
    v[i] = hexval(s[i]);
    if (v[i] < 0) return false;
  }
  *out = RGB(static_cast<uint8_t>(v[0] * 16 + v[1]),
             static_cast<uint8_t>(v[2] * 16 + v[3]),
             static_cast<uint8_t>(v[4] * 16 + v[5]));
  return true;
}

// round(pow(i/255, 2.6) * 255)
const uint8_t kGamma8[256] = {
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   1,   1,   1,   1,   1,   1,   1,
    1,   1,   1,   1,   1,   2,   2,   2,   2,   2,   2,   2,   2,   3,   3,   3,
    3,   3,   3,   4,   4,   4,   4,   4,   5,   5,   5,   5,   6,   6,   6,   6,
    7,   7,   7,   7,   8,   8,   8,   9,   9,   9,  10,  10,  10,  11,  11,  11,
   12,  12,  13,  13,  13,  14,  14,  15,  15,  16,  16,  17,  17,  18,  18,  19,
   19,  20,  20,  21,  21,  22,  22,  23,  24,  24,  25,  25,  26,  27,  27,  28,
   29,  29,  30,  31,  31,  32,  33,  34,  34,  35,  36,  37,  37,  38,  39,  40,
   40,  41,  42,  43,  44,  45,  46,  46,  47,  48,  49,  50,  51,  52,  53,  54,
   55,  56,  57,  58,  59,  60,  61,  62,  63,  64,  65,  66,  67,  68,  70,  71,
   72,  73,  74,  75,  77,  78,  79,  80,  81,  83,  84,  85,  86,  88,  89,  90,
   91,  93,  94,  95,  97,  98,  99, 101, 102, 104, 105, 107, 108, 110, 111, 113,
  114, 116, 117, 119, 120, 122, 123, 125, 127, 128, 130, 131, 133, 135, 136, 138,
  140, 142, 143, 145, 147, 149, 150, 152, 154, 156, 158, 160, 161, 163, 165, 167,
  169, 171, 173, 175, 177, 179, 181, 183, 185, 187, 189, 191, 193, 196, 198, 200,
  202, 204, 206, 209, 211, 213, 215, 218, 220, 222, 225, 227, 229, 232, 234, 236,
};

}  // namespace panel
