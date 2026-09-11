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
  rng_ = 0x1234567u;
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
        int32_t out;
        if (dither_ && exact < kDitherMinStep) {
          // Too dim to dither without being seen, and off means off.
          //
          // Two things at once. A pixel asked for black must be black: one that
          // went dark carrying a positive error would otherwise be pushed over
          // the threshold by the jitter and light for a frame, which across an
          // animating panel is a constant sparkle of LEDs that were asked to be
          // off. And a pixel asked for a twentieth of a step cannot be given one
          // at 100 fps without the pulses being far enough apart to count.
          //
          // Clearing the error matters as much as forcing the output. A residue
          // left behind fires later, on a pixel that has since been asked for
          // nothing at all.
          out = 0;
          err_[dst + c] = 0;
        } else if (dither_) {
          v += err_[dst + c];
          // A noisy comparator, not a fixed one.
          //
          // Plain sigma-delta on a constant input is exactly periodic: an
          // effective value of four 256ths fires once every sixty-four frames,
          // which at 100 fps is a 1.6 Hz blink. That is invisible at ordinary
          // levels, where the period is a handful of frames, and it is the
          // whole story at low brightness, where every dim pixel ends up in
          // that range. Jittering the threshold spreads the same average over a
          // noise floor instead of one low frequency.
          //
          // The error feedback is computed from the *unjittered* value, so the
          // accumulator still converges on the exact average — the noise moves
          // when a pulse happens, never how many.
          const int32_t jitter = static_cast<int32_t>(next_rand() & 0xFF);
          out = (v + jitter) >> 8;
          if (out < 0) out = 0;
          if (out > 255) out = 255;
          err_[dst + c] = static_cast<int16_t>(v - (out << 8));
          // Bounded, so a clamp at either end cannot let the accumulator run
          // away and take a whole second to come back.
          if (err_[dst + c] > 512) err_[dst + c] = 512;
          if (err_[dst + c] < -512) err_[dst + c] = -512;
        } else {
          out = v >> 8;
          if (out < 0) out = 0;
          if (out > 255) out = 255;
        }
        out_grb[dst + c] = static_cast<uint8_t>(out);
      }
    }
  }
  return st;
}

}  // namespace panel
