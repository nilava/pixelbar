// Turns a Framebuffer into the bytes that go out on the wire.
//
// Separate from Framebuffer because the dithering carries state from frame to
// frame, and that state belongs to the output, not to a picture. There are
// three framebuffers alive during a transition but only ever one output.
#pragma once
#include <cstdint>

#include "panel/config.h"
#include "panel/framebuffer.h"

namespace panel {

struct RenderStats {
  float power_scale = 1.0f;  // 1.0 means the cap did not engage
  float est_ma = 0.0f;
};

// Where dithering stops being a dimmer and starts being a flashing light.
//
// Dithering renders a fraction of an output step by firing that fraction of
// frames, so the pulse rate *is* the value. A channel at 6/256 fires once every
// 43 frames — 2.3 Hz — and no amount of jitter helps, because the pulses are
// sparse and separate rather than tonal. This is what made the accent colour
// appear to drift: gamma turns its blue component into 1 of 255, so at a low
// brightness blue fired alone every fiftieth frame on a pixel that should have
// been steady orange.
//
// Below this rate a channel is taken to zero, not reduced.
//
// A smooth roll-off was tried and is worse: reducing the average without
// reaching zero only makes the pulses rarer, and a rarer pulse is a more
// noticeable one. Measured, the roll-off turned a 4.7 Hz blink into a 0.4 Hz
// one. There is no smooth middle here — any average below the fuse rate
// produces visible pulses — so the only two honest values are zero and enough.
//
// The cost is a step at the very bottom of a fade, where a barely-visible pixel
// becomes an off one. That is a single edge in a place nothing much is
// happening, and it is a better trade than a light that blinks.
constexpr int32_t kDitherKnee = 256 * 30 / kFramesPerSecond;

class Renderer {
 public:
  Renderer() { reset_dither(); }

  void set_dither(bool on) { dither_ = on; }
  bool dither() const { return dither_; }
  void reset_dither();

  // Writes kNumLeds*3 GRB bytes in chain order. Not const: the fractional part
  // of each channel is carried into the next frame, so a value that sits
  // between two output bytes is shown as a mixture over time instead of being
  // rounded away. Without this, a slow fade at the default brightness has only
  // about 44 distinct steps and visibly staircases.
  RenderStats render(const Framebuffer& fb, uint8_t* out_grb, uint8_t brightness,
                     float max_ma, const Wiring& w);

  // A tiny PRNG used to break up the dither's periodicity. Seeded in
  // reset_dither, so a test that resets first still gets identical output.
  uint32_t rng_ = 1u;
  uint32_t next_rand() {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return rng_;
  }

 private:
  int16_t err_[kNumLeds * 3];  // residue in 1/256ths of an output step
  bool dither_ = true;
};

}  // namespace panel
