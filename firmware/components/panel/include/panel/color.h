// Colour types, HSV conversion and the gamma table.
#pragma once
#include <cstdint>

namespace panel {

struct RGB {
  uint8_t r = 0, g = 0, b = 0;

  constexpr RGB() = default;
  constexpr RGB(uint8_t r_, uint8_t g_, uint8_t b_) : r(r_), g(g_), b(b_) {}

  constexpr bool operator==(const RGB& o) const { return r == o.r && g == o.g && b == o.b; }
  constexpr bool lit() const { return r || g || b; }

  // Scale all channels by num/255.
  constexpr RGB scaled(uint8_t num) const {
    return RGB(static_cast<uint8_t>((r * num + 127) / 255),
               static_cast<uint8_t>((g * num + 127) / 255),
               static_cast<uint8_t>((b * num + 127) / 255));
  }
};

// h, s, v all in 0..1. h wraps.
RGB hsv(float h, float s, float v);

// Parse "#rrggbb" or "rrggbb". Returns false and leaves out untouched on bad input.
bool parse_hex_color(const char* s, RGB* out);

// 2.6 gamma, the usual choice for WS2812B. Exposed for tests.
extern const uint8_t kGamma8[256];

}  // namespace panel
