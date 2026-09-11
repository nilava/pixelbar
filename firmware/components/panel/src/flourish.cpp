#include "panel/flourish.h"

#include <cmath>
#include <cstring>

#include "panel/icons.h"
#include "panel/mini_font.h"
#include "panel/sprite.h"
#include "panel/screens.h"

namespace panel {
namespace {

void copy_word(char* dst, const char* src) {
  int i = 0;
  if (src) {
    for (; src[i] && i < 7; ++i) dst[i] = src[i];
  }
  dst[i] = 0;
}

// A highlight travelling across the field, sheared by radius so it reads as a
// band sweeping round a disk rather than a bar sliding sideways. The same
// geometry the view cycle turns on, reused for a much smaller thing.
void sweep(Framebuffer& fb, float p, RGB c) {
  if (p < 0.0f || p > 1.0f) return;
  const float head = p * (kWidth + 10.0f) - 5.0f;
  for (int x = 0; x < kWidth; ++x) {
    for (int y = 0; y < kHeight; ++y) {
      const float shear = (y - 3.5f) * 0.8f;
      const float d = std::fabs(static_cast<float>(x) - (head + shear));
      if (d > 2.5f) continue;
      const float k = 1.0f - d / 2.5f;
      fb.add_scaled(x, y, c, k * k * 0.55f);
    }
  }
}

}  // namespace

void Flourish::done(RGB color, const char* word, double now_s) {
  kind_ = FlourishKind::Done;
  color_ = color;
  copy_word(word_, word);
  t0_ = now_s;
}

void Flourish::toast(RGB color, const char* word, const Icon* icon, double now_s) {
  kind_ = FlourishKind::Toast;
  color_ = color;
  icon_ = icon;
  copy_word(word_, word);
  t0_ = now_s;
}

void Flourish::bar(RGB color, float value, double now_s) {
  // Re-arming refreshes rather than restarts: a knob turned steadily would
  // otherwise replay the entrance on every detent and never settle.
  color_ = color;
  value_ = clamp01(value);
  kind_ = FlourishKind::Bar;
  t0_ = now_s;
}

float Flourish::seconds() const {
  switch (kind_) {
    case FlourishKind::Done: return kDoneSeconds;
    case FlourishKind::Toast: return kToastSeconds;
    case FlourishKind::Bar: return kBarSeconds;
    default: return 0.0f;
  }
}

bool Flourish::active(double now_s) const {
  return kind_ != FlourishKind::None && (now_s - t0_) < seconds();
}

bool Flourish::opaque(double now_s) const {
  // Only Done covers the panel, and only until it starts fading out.
  if (kind_ != FlourishKind::Done) return false;
  const float t = static_cast<float>(now_s - t0_);
  return t >= 0.0f && t < kDoneHoldS;
}

void Flourish::draw(Framebuffer& fb, const Anim& a) {
  if (!active(a.t)) {
    if (kind_ != FlourishKind::None && (a.t - t0_) >= seconds()) kind_ = FlourishKind::None;
    return;
  }
  const float t = static_cast<float>(a.t - t0_);

  switch (kind_) {
    case FlourishKind::Done: {
      // Four beats: flash, settle, hold, fade.
      //
      // The fade blends out rather than filling toward black, so what it
      // uncovers is the screen underneath. Filling to black instead ends the
      // moment on a hard cut from an empty panel to a lit one, which undoes
      // most of what the fade was for.
      float alpha;   // how much of the panel the field still owns
      float white;   // how much of the field is still the flash
      float reveal;  // how far the check has drawn itself on
      if (t < kDoneFlashS) {
        alpha = 1.0f;
        white = 1.0f;
        reveal = 0.0f;
      } else if (t < kDoneSettleS) {
        const float u = (t - kDoneFlashS) / (kDoneSettleS - kDoneFlashS);
        alpha = 1.0f;
        white = 1.0f - ease::out_cubic(u);
        reveal = u;
      } else if (t < kDoneHoldS) {
        alpha = 1.0f;
        white = 0.0f;
        reveal = 1.0f;
      } else {
        const float u = (t - kDoneHoldS) / (kDoneSeconds - kDoneHoldS);
        alpha = 1.0f - ease::in_out_sine(u);
        white = 0.0f;
        reveal = 1.0f;
      }

      const RGB field =
          lerp_rgb(color_, RGB(255, 255, 255), white)
              .scaled(static_cast<uint8_t>(kFlashLevel * 255.0f + 0.5f));
      if (alpha >= 0.999f) {
        fb.fill(field);
      } else {
        for (int y = 0; y < kHeight; ++y)
          for (int x = 0; x < kWidth; ++x) fb.blend(x, y, field, alpha);
      }

      // A highlight crosses the field once, during the hold. It is what stops
      // a second of flat colour from looking like a stuck frame.
      if (t >= kDoneSettleS && t < kDoneHoldS) {
        const float u = (t - kDoneSettleS) / (kDoneHoldS - kDoneSettleS);
        sweep(fb, u, lerp_rgb(color_, RGB(255, 255, 255), 0.7f));
      }

      // The check and the word are cut out of the field rather than drawn on
      // it, so they read as holes in a lit surface. The check draws itself on
      // during the settle, which is what gives the moment its beat. Both are
      // knocked out at the field's own alpha, so they leave with it instead of
      // hanging on as black marks over the screen beneath.
      if (reveal > 0.0f && alpha > 0.0f) {
        const float p = clamp01(reveal) * kStrokeCheck.n;
        const int full = static_cast<int>(p);
        for (int i = 0; i < kStrokeCheck.n && i <= full; ++i) {
          const int px = (kStrokeCheck.pts[i] >> 4) & 0xF;
          const int py = kStrokeCheck.pts[i] & 0xF;
          const float k = (i == full) ? (p - full) : 1.0f;
          fb.blend(kIconX + px, py, RGB(0, 0, 0), k * alpha);
        }
      }
      if (t >= kDoneSettleS && word_[0] && alpha > 0.0f) {
        mini_draw_text_squashed(fb, kLabelX, kMiniLabelBox, 1, word_, RGB(0, 0, 0),
                                1.0f, alpha, Blend::Over);
      }
      break;
    }

    case FlourishKind::Toast: {
      // Rises into place, holds, drops away. The screen underneath is dimmed
      // rather than replaced, so you keep your place in whatever you were doing.
      const float u = t / kToastSeconds;
      float in = 1.0f;
      if (u < 0.15f) {
        in = ease::out_cubic(u / 0.15f);
      } else if (u > 0.82f) {
        in = 1.0f - ease::in_out_sine((u - 0.82f) / 0.18f);
      }
      const uint8_t dim = static_cast<uint8_t>((1.0f - 0.75f * in) * 255.0f);
      for (int y = 0; y < kHeight; ++y)
        for (int x = 0; x < kWidth; ++x) fb.set(x, y, fb.get(x, y).scaled(dim));

      const RGB c = color_.scaled(static_cast<uint8_t>(in * 255.0f + 0.5f));
      if (icon_) draw_icon_scaled(fb, kIconX, 0, *icon_, c, in, in, in);
      if (word_[0])
        mini_draw_text_squashed(fb, kLabelX, kMiniLabelBox, 1, word_, c, in, in);
      break;
    }

    case FlourishKind::Bar: {
      // A value being dragged. It sits on the bottom two rows over a dimmed
      // screen, so you can see what you are changing while you change it.
      const float u = t / kBarSeconds;
      float in = 1.0f;
      if (u < 0.12f) {
        in = ease::out_cubic(u / 0.12f);
      } else if (u > 0.80f) {
        in = 1.0f - ease::in_out_sine((u - 0.80f) / 0.20f);
      }
      const uint8_t dim = static_cast<uint8_t>((1.0f - 0.55f * in) * 255.0f);
      for (int y = 0; y < kHeight; ++y)
        for (int x = 0; x < kWidth; ++x) fb.set(x, y, fb.get(x, y).scaled(dim));

      const RGB on = color_.scaled(static_cast<uint8_t>(in * 255.0f + 0.5f));
      const RGB off = color_.scaled(static_cast<uint8_t>(in * 26.0f + 0.5f));
      draw_bar_aa(fb, 6, 7, value_, on, off);
      break;
    }

    default:
      break;
  }
}

}  // namespace panel
