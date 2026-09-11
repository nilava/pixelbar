#include "panel/anim.h"

#include <cmath>

namespace panel {
namespace {

constexpr int kSinBits = 8;
constexpr int kSinSize = 1 << kSinBits;  // 256

// sin(2*pi*i/256) scaled to int16. Built once on first use rather than stored
// as a literal table, because it is 512 bytes either way and this cannot drift
// from the comment describing it.
struct SinTable {
  int16_t v[kSinSize + 1];  // one extra entry so interpolation never wraps
  SinTable() {
    for (int i = 0; i <= kSinSize; ++i) {
      v[i] = static_cast<int16_t>(
          std::lround(std::sin(2.0 * 3.14159265358979323846 * i / kSinSize) * 32767.0));
    }
  }
};
const SinTable kSin;

}  // namespace

// ---------------------------------------------------------------- time

float elapsed_s(micros_t now, micros_t prev, float clamp_s) {
  // Unsigned subtraction is correct across the 32-bit wrap.
  const micros_t d = now - prev;
  float s = static_cast<float>(d) * 1e-6f;
  if (s > clamp_s) s = clamp_s;
  return s;
}

float Anim::phase(float period_s) const {
  if (period_s <= 0.0f) return 0.0f;
  const double cycles = t / period_s;
  return static_cast<float>(cycles - std::floor(cycles));
}

float Anim::wave(float period_s) const { return 0.5f + 0.5f * fast_sin(phase(period_s)); }

float Anim::pingpong(float period_s) const {
  const float p = phase(period_s);
  return p < 0.5f ? p * 2.0f : (1.0f - p) * 2.0f;
}

Anim FrameClock::tick(micros_t now_us) {
  Anim a;
  if (!started_) {
    started_ = true;
  } else {
    a.dt = elapsed_s(now_us, last_us_);
  }
  last_us_ = now_us;
  now_s_ += a.dt;
  a.t = now_s_;
  return a;
}

void FrameClock::reset() {
  started_ = false;
  last_us_ = 0;
  now_s_ = 0.0;
}

void FpsMeter::tick(micros_t now_us) {
  if (!started_) {
    started_ = true;
    last_us_ = now_us;
    return;
  }
  const float ms = static_cast<float>(now_us - last_us_) * 1e-3f;
  last_us_ = now_us;
  ++frames_;
  if (ms > max_ms_) max_ms_ = ms;
  // Exponential average over roughly a second.
  const float k = 0.02f;
  avg_ms_ = (avg_ms_ == 0.0f) ? ms : avg_ms_ + (ms - avg_ms_) * k;
  fps_ = avg_ms_ > 0.0f ? 1000.0f / avg_ms_ : 0.0f;
}

// ---------------------------------------------------------------- scalars

float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

float lerp(float a, float b, float t) { return a + (b - a) * t; }

float wrap01(float v) { return v - std::floor(v); }

RGB lerp_rgb(RGB a, RGB b, float t) {
  const float u = clamp01(t);
  auto mix = [u](uint8_t x, uint8_t y) {
    return static_cast<uint8_t>(x + (static_cast<float>(y) - x) * u + 0.5f);
  };
  return RGB(mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b));
}

// ---------------------------------------------------------------- trig

float fast_sin(float turns) {
  const float p = wrap01(turns) * kSinSize;
  const int i = static_cast<int>(p);
  const float f = p - i;
  const float a = kSin.v[i];
  const float b = kSin.v[i + 1];
  return (a + (b - a) * f) * (1.0f / 32767.0f);
}

float fast_cos(float turns) { return fast_sin(turns + 0.25f); }

float breathe(double t_s, float period_s) {
  if (period_s <= 0.0f) return 0.0f;
  const double c = t_s / period_s;
  return 0.5f + 0.5f * fast_sin(static_cast<float>(c - std::floor(c)));
}

float saw(double t_s, float period_s) {
  if (period_s <= 0.0f) return 0.0f;
  const double c = t_s / period_s;
  return static_cast<float>(c - std::floor(c));
}

float tri(double t_s, float period_s) {
  const float p = saw(t_s, period_s);
  return p < 0.5f ? p * 2.0f : (1.0f - p) * 2.0f;
}

// ---------------------------------------------------------------- easing

namespace ease {

float linear(float t) { return clamp01(t); }

float in_out_sine(float t) {
  const float u = clamp01(t);
  return 0.5f - 0.5f * fast_cos(u * 0.5f);
}

float out_cubic(float t) {
  const float u = 1.0f - clamp01(t);
  return 1.0f - u * u * u;
}

float in_out_cubic(float t) {
  const float u = clamp01(t);
  if (u < 0.5f) return 4.0f * u * u * u;
  const float f = -2.0f * u + 2.0f;
  return 1.0f - f * f * f / 2.0f;
}

float out_quint(float t) {
  const float u = 1.0f - clamp01(t);
  return 1.0f - u * u * u * u * u;
}

float out_back(float t) {
  constexpr float c1 = 1.70158f;
  constexpr float c3 = c1 + 1.0f;
  const float u = clamp01(t) - 1.0f;
  return 1.0f + c3 * u * u * u + c1 * u * u;
}

float out_elastic(float t) {
  const float u = clamp01(t);
  if (u <= 0.0f) return 0.0f;
  if (u >= 1.0f) return 1.0f;
  // 2^(-10u) * sin((10u - 0.75) * 2pi/3) + 1, with the sine from the table.
  const float decay = std::exp2(-10.0f * u);
  return decay * fast_sin((10.0f * u - 0.75f) / 3.0f) + 1.0f;
}

}  // namespace ease

// ---------------------------------------------------------------- smoothing

float Smoothed::update(float dt_s) {
  if (tau_ <= 0.0f || dt_s <= 0.0f) {
    if (tau_ <= 0.0f) v_ = target_;
    return v_;
  }
  // 1 - exp(-dt/tau): the same wall-clock time gives the same result at any
  // frame rate, which is what keeps easing identical at 60 and 100 fps.
  const float k = 1.0f - std::exp2(-dt_s / tau_ * 1.442695f);  // exp(-x) == exp2(-x*log2e)
  v_ += (target_ - v_) * k;
  return v_;
}

bool Smoothed::settled(float eps) const {
  const float d = v_ - target_;
  return (d < 0 ? -d : d) <= eps;
}

void SmoothedRGB::snap(RGB c) {
  r.snap(c.r);
  g.snap(c.g);
  b.snap(c.b);
}

void SmoothedRGB::set_target(RGB c) {
  r.set_target(c.r);
  g.set_target(c.g);
  b.set_target(c.b);
}

RGB SmoothedRGB::update(float dt_s) {
  r.update(dt_s);
  g.update(dt_s);
  b.update(dt_s);
  return value();
}

RGB SmoothedRGB::value() const {
  auto to8 = [](float v) {
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    return static_cast<uint8_t>(v + 0.5f);
  };
  return RGB(to8(r.value()), to8(g.value()), to8(b.value()));
}

}  // namespace panel
