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

// ---------------------------------------------------------------- sub-pixel

static inline uint8_t add_sat(uint8_t a, float add) {
  const float v = a + add;
  return v <= 0.0f ? a : (v >= 255.0f ? 255 : static_cast<uint8_t>(v + 0.5f));
}

void Framebuffer::add_scaled(int x, int y, RGB c, float coverage) {
  if (!in_bounds(x, y) || coverage <= 0.0f) return;
  if (coverage > 1.0f) coverage = 1.0f;
  RGB& p = px_[y * kWidth + x];
  p.r = add_sat(p.r, c.r * coverage);
  p.g = add_sat(p.g, c.g * coverage);
  p.b = add_sat(p.b, c.b * coverage);
}

void Framebuffer::blend(int x, int y, RGB c, float coverage) {
  if (!in_bounds(x, y) || coverage <= 0.0f) return;
  if (coverage > 1.0f) coverage = 1.0f;
  RGB& p = px_[y * kWidth + x];
  const float k = 1.0f - coverage;
  p.r = static_cast<uint8_t>(p.r * k + c.r * coverage + 0.5f);
  p.g = static_cast<uint8_t>(p.g * k + c.g * coverage + 0.5f);
  p.b = static_cast<uint8_t>(p.b * k + c.b * coverage + 0.5f);
}

void Framebuffer::set_aa(float x, int y, RGB c, float coverage, Blend b) {
  const float fx = std::floor(x);
  const int ix = static_cast<int>(fx);
  const float f = x - fx;
  const float left = coverage * (1.0f - f);
  const float right = coverage * f;
  if (b == Blend::Add) {
    add_scaled(ix, y, c, left);
    add_scaled(ix + 1, y, c, right);
  } else {
    blend(ix, y, c, left);
    blend(ix + 1, y, c, right);
  }
}

void Framebuffer::set_aa2(float x, float y, RGB c, float coverage, Blend b) {
  const float fy = std::floor(y);
  const int iy = static_cast<int>(fy);
  const float g = y - fy;
  set_aa(x, iy, c, coverage * (1.0f - g), b);
  set_aa(x, iy + 1, c, coverage * g, b);
}

void Framebuffer::span_h(float x0, float x1, int y, RGB c, Blend b) {
  if (x1 <= x0) return;
  const int first = static_cast<int>(std::floor(x0));
  const int last = static_cast<int>(std::ceil(x1)) - 1;
  for (int x = first; x <= last; ++x) {
    const float l = std::max(x0, static_cast<float>(x));
    const float r = std::min(x1, static_cast<float>(x + 1));
    const float cov = r - l;
    if (cov <= 0.0f) continue;
    if (b == Blend::Add) {
      add_scaled(x, y, c, cov);
    } else {
      blend(x, y, c, cov);
    }
  }
}

void blit_offset(Framebuffer& dst, const Framebuffer& src, float dx, float dy) {
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      const float sx = x - dx;
      const float sy = y - dy;
      const int x0 = static_cast<int>(std::floor(sx));
      const int y0 = static_cast<int>(std::floor(sy));
      const float fx = sx - x0;
      const float fy = sy - y0;
      // Four taps, each contributing its bilinear weight.
      const float w[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
      const int px[4] = {x0, x0 + 1, x0, x0 + 1};
      const int py[4] = {y0, y0, y0 + 1, y0 + 1};
      float r = 0, g = 0, b = 0;
      for (int i = 0; i < 4; ++i) {
        if (w[i] <= 0.0f || !in_bounds(px[i], py[i])) continue;
        const RGB c = src.get(px[i], py[i]);
        r += c.r * w[i];
        g += c.g * w[i];
        b += c.b * w[i];
      }
      dst.add_scaled(x, y, RGB(static_cast<uint8_t>(std::min(255.0f, r + 0.5f)),
                               static_cast<uint8_t>(std::min(255.0f, g + 0.5f)),
                               static_cast<uint8_t>(std::min(255.0f, b + 0.5f))),
                     1.0f);
    }
  }
}

void cross_fade(Framebuffer& dst, const Framebuffer& a, const Framebuffer& b, float u) {
  if (u < 0.0f) u = 0.0f;
  if (u > 1.0f) u = 1.0f;
  const float k = 1.0f - u;
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      const RGB ca = a.get(x, y), cb = b.get(x, y);
      dst.set(x, y, RGB(static_cast<uint8_t>(ca.r * k + cb.r * u + 0.5f),
                        static_cast<uint8_t>(ca.g * k + cb.g * u + 0.5f),
                        static_cast<uint8_t>(ca.b * k + cb.b * u + 0.5f)));
    }
  }
}

}  // namespace panel
