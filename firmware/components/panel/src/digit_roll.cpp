#include "panel/digit_roll.h"

#include <cmath>

#include "panel/anim.h"
#include "panel/font.h"
#include "panel/framebuffer.h"
#include "panel/geometry.h"
#include "panel/sprite.h"

namespace panel {
namespace {

constexpr int kTinyAdvance = kTinyW + 1;
constexpr int kColonWidth = 2;

// The digits sit on the rim of a cylinder whose axis runs across the panel.
// Radius is half the glyph height, so consecutive faces meet edge to edge.
constexpr float kWheelRadius = kTinyH * 0.5f;

Sprite digit_sprite(char c) {
  const uint8_t* d = tiny_digit(c);
  if (!d) return Sprite{};
  return Sprite{d, kTinyW, kTinyH, kTinyW - 1};
}

// One face on the rim, at rim angle `turns` from the front. A face presents
// cos of its area and sits sin of the radius away from the axis, which is the
// whole of the projection: no other term is needed to make it look solid.
void draw_wheel_face(Framebuffer& fb, char c, float cx, float axis_y, float turns,
                     int8_t dir, RGB color, int band_y0, int band_y1) {
  const Sprite s = digit_sprite(c);
  if (!s.rows) return;
  const float sy = fast_cos(turns);
  if (sy <= 0.01f) return;  // edge-on or facing away
  const float cy = axis_y + kWheelRadius * fast_sin(turns) * static_cast<float>(dir);
  // Lambert with a floor. The floor exists because eight rows cannot afford a
  // face that dims in true proportion to its projected area.
  const float bright = 0.55f + 0.45f * sy;
  draw_sprite_scaled(fb, s, cx, cy, 1.0f, sy, color, bright, band_y0, band_y1,
                     Ink::Normalised, Blend::Add);
}

}  // namespace

void DigitRoll::set(char c, double now_s, int8_t direction) {
  if (c == to) return;
  from = to;
  to = c;
  t0 = now_s;
  dir = direction;
  active = true;
}

bool DigitRoll::running(double now_s) const {
  return active && (now_s - t0) < kRollSeconds;
}

float DigitRoll::progress(double now_s) const {
  if (!active) return 0.0f;
  const double d = now_s - t0;
  if (d <= 0.0) return 0.0f;
  if (d >= kRollSeconds) return 1.0f;
  return static_cast<float>(d / kRollSeconds);
}

void draw_rolling_digit(Framebuffer& fb, int x, int y, const DigitRoll& r, double now_s,
                        RGB color) {
  const float cx = static_cast<float>(x) + kTinyW * 0.5f;
  const float axis_y = static_cast<float>(y) + kTinyH * 0.5f;
  const int band0 = y, band1 = y + kTinyH;

  if (!r.running(now_s)) {
    draw_wheel_face(fb, r.to, cx, axis_y, 0.0f, r.dir, color, band0, band1);
    return;
  }

  // A quarter turn brings the next face to the front. out_cubic, not out_quint:
  // a quintic puts the whole turn in the first third of the time and the wheel
  // spends the rest of it already settled, so the foreshortening that sells the
  // rotation is over before the eye catches it.
  const float u = ease::out_cubic(r.progress(now_s));
  const float theta = u * 0.25f;

  // Both faces are on screen for the whole turn — the outgoing one foreshortens
  // and rides away from the axis while the incoming one rises into it. Drawing
  // them together is what produces the doubled, squeezed look mid-turn, and it
  // is the thing that distinguishes a wheel from a crossfade.
  draw_wheel_face(fb, r.from, cx, axis_y, theta, r.dir, color, band0, band1);
  draw_wheel_face(fb, r.to, cx, axis_y, theta - 0.25f, r.dir, color, band0, band1);

  // The rim, as the join between the two faces sweeps past. Without it the
  // middle of the turn is a gap and the movement loses its centre.
  //
  // The highlight is pushed toward white rather than drawn in the face colour.
  // A specular is the colour of the light, not of the surface, and adding the
  // accent to itself only drives two channels into clipping: orange on orange
  // reads as yellow, which looks like a colour change rather than a glint.
  const float d = std::fabs(theta - 0.125f);
  if (d < 0.085f) {
    const float k = 1.0f - d / 0.085f;
    const uint8_t k8 = static_cast<uint8_t>(k * k * 200.0f + 0.5f);
    const RGB spec = lerp_rgb(color, RGB(255, 255, 255), 0.6f);
    const float ry = axis_y + kWheelRadius * fast_sin(theta - 0.125f) *
                                  static_cast<float>(r.dir);
    const int row = static_cast<int>(std::floor(ry));
    if (row >= band0 && row < band1) {
      fb.span_h(cx - kTinyW * 0.5f, cx + kTinyW * 0.5f, row, spec.scaled(k8),
                Blend::Add);
    }
  }
}

void draw_pair_face_anim(Framebuffer& fb, PairFaceAnim& anim, int left, int right,
                         bool show_colon, RGB color, double now_s) {
  auto clamp99 = [](int v) { return v < 0 ? 0 : (v > 99 ? 99 : v); };
  const int a = clamp99(left);
  const int b = clamp99(right);
  const char want[4] = {
      static_cast<char>('0' + a / 10), static_cast<char>('0' + a % 10),
      static_cast<char>('0' + b / 10), static_cast<char>('0' + b % 10)};

  // A countdown rolls down, a clock rolls up. Work it out from the pair value
  // so the direction is right without the caller having to say.
  int8_t dir = -1;
  if (anim.primed) {
    const int prev = (anim.d[0].to - '0') * 1000 + (anim.d[1].to - '0') * 100 +
                     (anim.d[2].to - '0') * 10 + (anim.d[3].to - '0');
    const int cur = (want[0] - '0') * 1000 + (want[1] - '0') * 100 +
                    (want[2] - '0') * 10 + (want[3] - '0');
    dir = (cur > prev) ? 1 : -1;
  }

  if (!anim.primed) {
    for (int i = 0; i < 4; ++i) {
      anim.d[i].to = want[i];
      anim.d[i].from = want[i];
      anim.d[i].active = false;
    }
    anim.primed = true;
  } else {
    for (int i = 0; i < 4; ++i) anim.d[i].set(want[i], now_s, dir);
  }

  int x = (kWidth - (4 * kTinyAdvance + kColonWidth - 1)) / 2;
  const int y = 1;
  draw_rolling_digit(fb, x, y, anim.d[0], now_s, color); x += kTinyAdvance;
  draw_rolling_digit(fb, x, y, anim.d[1], now_s, color); x += kTinyAdvance;
  if (show_colon) {
    fb.set(x, y + 1, color);
    fb.set(x, y + 3, color);
  }
  x += kColonWidth;
  draw_rolling_digit(fb, x, y, anim.d[2], now_s, color); x += kTinyAdvance;
  draw_rolling_digit(fb, x, y, anim.d[3], now_s, color);
}

}  // namespace panel
