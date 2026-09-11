// A 24x8 RGB framebuffer that renders into WS2812B wire order.
//
// Nothing here touches ESP-IDF, so the whole drawing and pattern layer builds
// and runs on the host for tests and for the PNG preview tool.
#pragma once
#include <cstdint>

#include "panel/color.h"
#include "panel/geometry.h"

namespace panel {

// Current a single LED draws with all three channels at full, in milliamps.
// 20 mA per channel is the WS2812B datasheet figure.
constexpr float kFullWhiteMaPerLed = 60.0f;
// Quiescent draw of the LED's own controller, milliamps.
constexpr float kIdleMaPerLed = 1.0f;

class Framebuffer {
 public:
  void clear() { fill(RGB(0, 0, 0)); }
  void fill(RGB c);

  // Out-of-range coordinates are ignored, which keeps scrolling code simple.
  void set(int x, int y, RGB c) {
    if (in_bounds(x, y)) px_[y * kWidth + x] = c;
  }
  // Add to a pixel, saturating. Useful for overlapping effects.
  void add(int x, int y, RGB c);

  RGB get(int x, int y) const {
    return in_bounds(x, y) ? px_[y * kWidth + x] : RGB();
  }

  void fill_rect(int x, int y, int w, int h, RGB c);

  // Estimated supply current for the current contents at this brightness.
  float estimate_ma(uint8_t brightness) const;

  // Write 192*3 bytes of GRB, in chain order, applying gamma, brightness and
  // the power cap. Returns the scale factor the power cap applied (1.0 = none),
  // so the caller can tell the user it is limiting.
  float render(uint8_t* out_grb, uint8_t brightness, float max_ma, const Wiring& w) const;

  const RGB* pixels() const { return px_; }

 private:
  RGB px_[kNumLeds];
};

}  // namespace panel
