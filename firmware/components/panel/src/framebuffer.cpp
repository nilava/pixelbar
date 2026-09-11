#include "panel/framebuffer.h"

#include <algorithm>
#include <cmath>

namespace panel {

void Framebuffer::fill(RGB c) {
  for (int i = 0; i < kNumLeds; ++i) px_[i] = c;
}

void Framebuffer::add(int x, int y, RGB c) {
  if (!in_bounds(x, y)) return;
  RGB& p = px_[y * kWidth + x];
  p.r = static_cast<uint8_t>(std::min(255, p.r + c.r));
  p.g = static_cast<uint8_t>(std::min(255, p.g + c.g));
  p.b = static_cast<uint8_t>(std::min(255, p.b + c.b));
}

void Framebuffer::fill_rect(int x, int y, int w, int h, RGB c) {
  for (int yy = y; yy < y + h; ++yy)
    for (int xx = x; xx < x + w; ++xx) set(xx, yy, c);
}

// Sum of the gamma-corrected, brightness-scaled channels, converted to mA.
static float sum_ma(const RGB* px, uint8_t brightness) {
  uint32_t sum = 0;  // 0 .. 192*3*255
  for (int i = 0; i < kNumLeds; ++i) {
    sum += kGamma8[px[i].r];
    sum += kGamma8[px[i].g];
    sum += kGamma8[px[i].b];
  }
  const float duty = static_cast<float>(sum) * brightness / (255.0f * 255.0f);
  return duty * (kFullWhiteMaPerLed / 3.0f) + kNumLeds * kIdleMaPerLed;
}

float Framebuffer::estimate_ma(uint8_t brightness) const {
  return sum_ma(px_, brightness);
}

float Framebuffer::render(uint8_t* out_grb, uint8_t brightness, float max_ma,
                          const Wiring& w) const {
  float scale = 1.0f;
  if (max_ma > 0.0f) {
    const float ma = sum_ma(px_, brightness);
    // Only the LED duty is scalable; the idle draw is a floor we cannot reduce.
    const float idle = kNumLeds * kIdleMaPerLed;
    if (ma > max_ma && ma > idle) {
      scale = std::max(0.0f, (max_ma - idle)) / (ma - idle);
      scale = std::min(1.0f, scale);
    }
  }
  const float k = (brightness / 255.0f) * scale;

  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      const RGB& c = px_[y * kWidth + x];
      const int i = led_index(x, y, w) * 3;
      // WS2812B takes green first.
      out_grb[i + 0] = static_cast<uint8_t>(kGamma8[c.g] * k + 0.5f);
      out_grb[i + 1] = static_cast<uint8_t>(kGamma8[c.r] * k + 0.5f);
      out_grb[i + 2] = static_cast<uint8_t>(kGamma8[c.b] * k + 0.5f);
    }
  }
  return scale;
}

}  // namespace panel
