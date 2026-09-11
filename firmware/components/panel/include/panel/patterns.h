// The pattern engine: everything that draws into a Framebuffer over time.
//
// Timebase is a millisecond counter passed in by the caller, so the same code
// runs off esp_timer on the device and off a synthetic clock in tests.
#pragma once
#include <cstdint>

#include "panel/color.h"
#include "panel/framebuffer.h"

namespace panel {

enum class Pattern : uint8_t {
  Text = 0,
  Clock,
  Rainbow,
  Plasma,
  Sparkle,
  Solid,
  MapTest,  // diagnostic: confirms the Wiring config against real hardware
  Count,
};

const char* pattern_name(Pattern p);
// Case-insensitive lookup by name. Returns false if unknown.
bool pattern_from_name(const char* name, Pattern* out);

constexpr int kMaxText = 96;

struct Params {
  RGB color{255, 138, 31};
  float speed = 1.0f;      // multiplier on every pattern's rate
  char text[kMaxText] = "PIXELBAR";
  // Wall clock, supplied by the caller; the engine never reads the RTC itself.
  int hour = 0;
  int minute = 0;
  int second = 0;
};

// Deterministic PRNG so sparkle is reproducible in tests.
class Rng {
 public:
  explicit Rng(uint32_t seed = 0x1234567u) : s_(seed ? seed : 1u) {}
  uint32_t next() {
    s_ ^= s_ << 13;
    s_ ^= s_ >> 17;
    s_ ^= s_ << 5;
    return s_;
  }
  float unit() { return static_cast<float>(next() & 0xFFFFFF) / 16777216.0f; }

 private:
  uint32_t s_;
};

class Engine {
 public:
  Engine();

  void set_pattern(Pattern p);
  Pattern pattern() const { return pat_; }
  void set_text(const char* s);

  // Advances animation state to now_ms and draws one frame.
  void render(Framebuffer& fb, uint32_t now_ms);

  // Scroll offset in pixels, exposed for tests.
  float scroll_px() const { return scroll_; }
  // Seconds of animation time accumulated, scaled by speed.
  float anim_time() const { return t_; }

  Params params;

 private:
  void draw_text(Framebuffer& fb);
  void draw_clock(Framebuffer& fb);
  void draw_rainbow(Framebuffer& fb);
  void draw_plasma(Framebuffer& fb);
  void draw_sparkle(Framebuffer& fb, float dt);
  void draw_maptest(Framebuffer& fb);

  Pattern pat_ = Pattern::Text;
  bool started_ = false;
  uint32_t last_ms_ = 0;
  float t_ = 0.0f;       // animation seconds
  float scroll_ = 0.0f;  // text scroll offset in pixels
  float sparks_[kNumLeds] = {0};
  Rng rng_;
};

}  // namespace panel
