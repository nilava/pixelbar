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

// How a coverage-weighted write combines with what is already there.
//
// Add is the default for ink. Adjacent columns of one glyph drawn at a
// fractional x write complementary weights into the same LED, and they must
// sum: with Over, a solid stroke would dim to max(f, 1-f) and shimmer as it
// scrolls. Over is for anything that paints a background, such as a bar.
enum class Blend : uint8_t { Add, Over };

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

  // ------------------------------------------------ sub-pixel drawing
  //
  // Coverage is applied in the encoded, pre-gamma domain. That is not
  // photometrically linear, and it is deliberate: it is what makes edges look
  // right to the eye, and it is what every LED matrix library does. Do not
  // "fix" it to linear light without looking at the panel first.

  // Saturating add of c scaled by coverage.
  void add_scaled(int x, int y, RGB c, float coverage);
  // Source-over: p = p*(1-coverage) + c*coverage.
  void blend(int x, int y, RGB c, float coverage);

  // One point at a fractional column, split between the two LEDs it straddles.
  void set_aa(float x, int y, RGB c, float coverage = 1.0f, Blend b = Blend::Add);
  // Bilinear, four taps. For vertical motion and screen slides.
  void set_aa2(float x, float y, RGB c, float coverage = 1.0f, Blend b = Blend::Add);

  // A horizontal run over [x0, x1) with exact partial end pixels. The coverage
  // it writes sums to exactly x1-x0.
  void span_h(float x0, float x1, int y, RGB c, Blend b = Blend::Over);

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

// dst += src sampled at (x-dx, y-dy), bilinear. The workhorse of slides.
void blit_offset(Framebuffer& dst, const Framebuffer& src, float dx, float dy);

// The panel read as a radial strip of a turning disk: column 0 nearest the hub,
// column 23 at the rim.
//
// Content printed on a disk advances through the same *angle* at every radius,
// and therefore through a different *distance*. Unrolled, that is a vertical
// displacement proportional to the radius. It is the reason this feels like a
// physical record and a slide does not: the rim end whips past, the hub end
// crawls, and a glyph spanning several columns shears as it goes because its
// left edge is moving slower than its right.
//
// The hub does not sit at column 0 exactly. A true hub radius of zero would pin
// the left column in place forever, and there would be no movement there at all
// to fade out; a few columns of offset keeps the ratio dramatic while still
// letting the whole panel clear.
constexpr float kDiskHubRadius = 3.0f;

// `turn` is rows of displacement per unit radius.
//
// The rim would clear an eight-row panel at about 0.3, and that was the first
// choice — which was wrong, and is why the effect could not be seen. At that
// figure the hub end travels 1.4 rows, which over four hundred milliseconds is
// close enough to still that most of the panel was simply cross-fading. The
// visible part of a disk turning is the *difference* between the ends, and
// there is no difference to see if one end does not move.
//
// So the rim over-travels instead: it leaves in the first third and keeps
// going, which is exactly what the outer edge of a record does. At 0.85 the
// hub moves 2.5 rows and the rim 22 — a ratio approaching nine to one, and a
// shear that visibly slices the outgoing screen on its way past.
constexpr float kDiskClearTurn = 0.85f;

// dst += src displaced by the disk shear.
//
// The gain is radial for the same reason the displacement is. A column near the
// rim leaves the panel by travelling, so it can stay bright the whole way; a
// column near the hub cannot travel far enough to leave at all, so it has to be
// faded instead. One scalar gain for the whole panel would either strand the
// hub or dim the rim into a cross-fade, which is the failure this is avoiding.
void blit_disk(Framebuffer& dst, const Framebuffer& src, float turn,
               float gain_hub = 1.0f, float gain_rim = -1.0f);

// dst = a*(1-u) + b*u, per channel.
void cross_fade(Framebuffer& dst, const Framebuffer& a, const Framebuffer& b, float u);

}  // namespace panel
