// Animation maths: time, easing, smoothing and fast trigonometry.
//
// This is the leaf header of the panel component. It depends only on color.h,
// so everything else can build on it.
//
// The ESP32-C3 has no FPU, so every float operation here is emulated in
// software. That is why the trig is a table rather than libm: a per-pixel
// sinf costs on the order of a thousand cycles, and a 24x8 plasma frame needs
// hundreds of them.
#pragma once
#include <cstdint>

#include "panel/color.h"

namespace panel {

// ---------------------------------------------------------------- time

// Microsecond stamp. Deliberately 32 bit: unsigned subtraction stays correct
// across the wrap for any interval under 71.6 minutes, which is the same trick
// the old millisecond counter relied on, at a thousand times the resolution.
using micros_t = uint32_t;

// A stall must not teleport the animation forward.
constexpr float kMaxFrameDt = 0.25f;

// Wrap-safe elapsed seconds, clamped.
float elapsed_s(micros_t now, micros_t prev, float clamp_s = kMaxFrameDt);

// The timing handed to every draw call.
//
// dt drives easing and anything velocity-based. t drives cyclic effects, so
// their phase is continuous even when a screen appears mid-cycle. t is double
// because a float's 24-bit mantissa would quantise to milliseconds after an
// hour of uptime, which would reintroduce stepping for a subtler reason.
struct Anim {
  float dt = 0.0f;
  double t = 0.0;

  float phase(float period_s) const;     // 0..1 sawtooth
  float wave(float period_s) const;      // 0..1 sine
  float pingpong(float period_s) const;  // 0..1..0 triangle
};

// Turns Anim out of a microsecond counter, handling the first frame and stalls.
class FrameClock {
 public:
  Anim tick(micros_t now_us);  // dt is 0 on the very first call
  double now_s() const { return now_s_; }
  bool started() const { return started_; }
  void reset();

 private:
  bool started_ = false;
  micros_t last_us_ = 0;
  double now_s_ = 0.0;
};

// Measures the real frame cadence, so "100 fps" can be checked rather than
// assumed. The old code claimed 60 and actually ran at 62.5.
class FpsMeter {
 public:
  void tick(micros_t now_us);
  float fps() const { return fps_; }
  float frame_ms_avg() const { return avg_ms_; }
  float frame_ms_max() const { return max_ms_; }
  void reset_peak() { max_ms_ = 0.0f; }
  uint32_t frames() const { return frames_; }

 private:
  bool started_ = false;
  micros_t last_us_ = 0;
  float avg_ms_ = 0.0f;
  float max_ms_ = 0.0f;
  float fps_ = 0.0f;
  uint32_t frames_ = 0;
};

// ---------------------------------------------------------------- scalars

float clamp01(float v);
float lerp(float a, float b, float t);
float wrap01(float v);
RGB lerp_rgb(RGB a, RGB b, float t);

// ---------------------------------------------------------------- trig

// Angle in TURNS, not radians: fast_sin(0.25f) == 1. A 256-entry table with
// linear interpolation, accurate to about 1e-4, at a fraction of libm's cost.
float fast_sin(float turns);
float fast_cos(float turns);

// Cyclic helpers built on the table.
float breathe(double t_s, float period_s);  // 0..1 sine
float saw(double t_s, float period_s);      // 0..1 ramp
float tri(double t_s, float period_s);      // 0..1..0 triangle

// ---------------------------------------------------------------- easing

namespace ease {
float linear(float t);
float in_out_sine(float t);   // fades
float out_cubic(float t);     // slides
float in_out_cubic(float t);  // general purpose
float out_quint(float t);     // digit rolls: fast start, long settle
float out_back(float t);      // wipes, with a small overshoot
float out_elastic(float t);   // arrivals worth celebrating
}  // namespace ease

// ---------------------------------------------------------------- smoothing

// Exponential approach toward a target.
//
// Frame-rate independent by construction: the step is exp(-dt/tau), so the
// same wall-clock time produces the same result at 60 or 100 fps. Chosen over
// a spring because it can never overshoot a brightness into a visible flash.
class Smoothed {
 public:
  explicit Smoothed(float v = 0.0f, float tau_s = 0.12f)
      : v_(v), target_(v), tau_(tau_s) {}

  void snap(float v) { v_ = target_ = v; }
  void set_target(float v) { target_ = v; }
  float target() const { return target_; }
  void set_tau(float tau_s) { tau_ = tau_s; }

  float update(float dt_s);
  float value() const { return v_; }
  bool settled(float eps = 0.001f) const;

 private:
  float v_, target_, tau_;
};

struct SmoothedRGB {
  Smoothed r{0.0f, 0.12f}, g{0.0f, 0.12f}, b{0.0f, 0.12f};
  void snap(RGB c);
  void set_target(RGB c);
  RGB update(float dt_s);
  RGB value() const;
};

}  // namespace panel
