#include "panel/renderer.h"

#include <algorithm>

namespace panel {

void Renderer::reset_dither() {
  // A fixed decorrelation pattern, not zero and not random. If every channel
  // started level they would all flip on the same frame and the whole panel
  // would shimmer in lockstep, which is far more visible than the stepping
  // this is meant to remove. 149 is coprime with both 256 and 3, so
  // neighbouring LEDs and the three channels of one LED all dither out of
  // phase. Deterministic, so the output is reproducible in tests.
  for (int i = 0; i < kNumLeds * 3; ++i) err_[i] = static_cast<int16_t>((i * 149) & 0xFF);
}

RenderStats Renderer::render(const Framebuffer& fb, uint8_t* out_grb, uint8_t brightness,
                             float max_ma, const Wiring& w) {
  RenderStats st;

  // One pass to gamma-correct and total the draw at the same time.
  uint8_t gam[kNumLeds * 3];
  uint32_t sum = 0;
  const RGB* px = fb.pixels();
  for (int i = 0; i < kNumLeds; ++i) {
    const uint8_t r = kGamma8[px[i].r];
    const uint8_t g = kGamma8[px[i].g];
    const uint8_t b = kGamma8[px[i].b];
    gam[i * 3 + 0] = r;
    gam[i * 3 + 1] = g;
    gam[i * 3 + 2] = b;
    sum += r + g + b;
  }

  const float duty = static_cast<float>(sum) * brightness / (255.0f * 255.0f);
  const float idle = kNumLeds * kIdleMaPerLed;
  st.est_ma = duty * (kFullWhiteMaPerLed / 3.0f) + idle;

  float scale = 1.0f;
  if (max_ma > 0.0f && st.est_ma > max_ma && st.est_ma > idle) {
    scale = std::max(0.0f, max_ma - idle) / (st.est_ma - idle);
    scale = std::min(1.0f, scale);
  }
  st.power_scale = scale;

  // Fixed point so the inner loop has no float and is bit-identical on the
  // host and on the target.
  const int32_t k = static_cast<int32_t>((brightness / 255.0f) * scale * 65536.0f + 0.5f);

  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      const int src = (y * kWidth + x) * 3;
      const int dst = led_index(x, y, w) * 3;
      // WS2812B takes green first.
      const int order[3] = {src + 1, src + 0, src + 2};
      for (int c = 0; c < 3; ++c) {
        // Exact value in 1/256ths of an output step.
        const int32_t exact = (static_cast<int32_t>(gam[order[c]]) * k) >> 8;
        int32_t v = exact;
        if (dither_) v += err_[dst + c];
        int32_t out = v >> 8;
        if (out < 0) out = 0;
        if (out > 255) out = 255;
        if (dither_) err_[dst + c] = static_cast<int16_t>(v - (out << 8));
        out_grb[dst + c] = static_cast<uint8_t>(out);
      }
    }
  }
  return st;
}

}  // namespace panel
