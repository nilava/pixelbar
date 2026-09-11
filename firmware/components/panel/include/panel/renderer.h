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

// The dimmest value temporal dithering can carry without being seen.
//
// Dithering renders a fraction of an output step by firing that fraction of
// frames. The pulses have to be close enough together to fuse: a value of
// 12/256 fires once every 21 frames, which at 100 fps is a 4.7 Hz blink and is
// not a compromise, it is a flashing light. Below about 30 Hz there is nothing
// to be done about that at one bit and one hundred frames a second — jitter
// spreads the frequency but the pulses are still sparse and separate.
//
// So below this, values round down instead. It costs the dimmest sliver of the
// range at very low brightness, where there is almost no detail to lose, and it
// buys a panel that sits still.
constexpr int32_t kDitherMinStep = 256 * 30 / kFramesPerSecond;

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
