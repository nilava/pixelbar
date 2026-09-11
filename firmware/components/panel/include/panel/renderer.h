// Turns a Framebuffer into the bytes that go out on the wire.
//
// Separate from Framebuffer because the dithering carries state from frame to
// frame, and that state belongs to the output, not to a picture. There are
// three framebuffers alive during a transition but only ever one output.
#pragma once
#include <cstdint>

#include "panel/framebuffer.h"

namespace panel {

struct RenderStats {
  float power_scale = 1.0f;  // 1.0 means the cap did not engage
  float est_ma = 0.0f;
};

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

 private:
  int16_t err_[kNumLeds * 3];  // residue in 1/256ths of an output step
  bool dither_ = true;
};

}  // namespace panel
