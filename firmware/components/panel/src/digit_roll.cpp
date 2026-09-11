#include "panel/digit_roll.h"

#include <cmath>

#include "panel/anim.h"
#include "panel/font.h"
#include "panel/framebuffer.h"
#include "panel/geometry.h"

namespace panel {
namespace {

constexpr int kTinyAdvance = kTinyW + 1;
constexpr int kColonWidth = 2;

// Draws one 3x5 digit offset vertically by dy, emitting only the rows that
// still fall inside the band [0, kTinyH). This is the clipping.
void draw_digit_offset(Framebuffer& fb, int x, int y, char c, float dy, RGB color,
                       float alpha) {
  const uint8_t* d = tiny_digit(c);
  if (!d || alpha <= 0.0f) return;
  for (int row = 0; row < kTinyH; ++row) {
    const float ry = row + dy;  // destination row, in band-local coordinates
    if (ry <= -1.0f || ry >= kTinyH) continue;
    // Split the row between the two it straddles, and drop any tap that falls
    // outside the band. Doing the vertical split here rather than with set_aa2
    // is what guarantees a roll never touches a neighbouring row: set_aa2
    // would happily write to band-local row -1.
    const float base = std::floor(ry);
    const int r0 = static_cast<int>(base);
    const float f = ry - base;
    const int rows[2] = {r0, r0 + 1};
    const float w[2] = {1.0f - f, f};
    for (int col = 0; col < kTinyW; ++col) {
      if (!(d[row] & (1 << (kTinyW - 1 - col)))) continue;
      for (int k = 0; k < 2; ++k) {
        if (rows[k] < 0 || rows[k] >= kTinyH || w[k] <= 0.0f) continue;
        fb.add_scaled(x + col, y + rows[k], color, alpha * w[k]);
      }
    }
  }
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

void draw_rolling_digit(Framebuffer& fb, int x, int y, const DigitRoll& r, double now_s,
                        RGB color) {
  if (!r.running(now_s)) {
    draw_digit_offset(fb, x, y, r.to, 0.0f, color, 1.0f);
    return;
  }
  const float u = ease::out_quint(static_cast<float>((now_s - r.t0) / kRollSeconds));
  const float span = static_cast<float>(kTinyH);
  // The outgoing digit slides away, the incoming one arrives from the far side.
  draw_digit_offset(fb, x, y, r.from, u * span * r.dir, color, 1.0f - u * 0.2f);
  draw_digit_offset(fb, x, y, r.to, (u - 1.0f) * span * r.dir, color, 1.0f);
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
