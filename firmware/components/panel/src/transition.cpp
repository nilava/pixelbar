#include "panel/transition.h"

#include <cmath>

namespace panel {

const char* transition_name(TransitionKind k) {
  switch (k) {
    case TransitionKind::None: return "none";
    case TransitionKind::SlideLeft: return "slideleft";
    case TransitionKind::SlideRight: return "slideright";
    case TransitionKind::WipeUp: return "wipeup";
    case TransitionKind::WipeDown: return "wipedown";
    case TransitionKind::Dissolve: return "dissolve";
    case TransitionKind::Fade: return "fade";
    default: return "?";
  }
}

namespace {

bool is_adjust(Screen s) {
  return s == Screen::Brightness || s == Screen::ColorPick || s == Screen::TimerSet;
}

// Position of a screen in the view cycle, or -1 if it is not part of it.
int view_order(Screen s) {
  switch (s) {
    case Screen::Status: return 0;
    case Screen::Clock: return 1;
    case Screen::Timer: return 2;
    default: return -1;
  }
}

}  // namespace

TransitionKind ScreenManager::kind_for(Screen from, Screen to) {
  if (from == to) return TransitionKind::None;
  if (to == Screen::Sleep || from == Screen::Sleep) return TransitionKind::Fade;
  if (from == Screen::Booting) return TransitionKind::Fade;
  if (is_adjust(to)) return TransitionKind::WipeUp;    // a drawer opening
  if (is_adjust(from)) return TransitionKind::WipeDown;  // and closing

  const int a = view_order(from), b = view_order(to);
  if (a >= 0 && b >= 0) {
    // Wrapping from the last view back to the first should still read forward.
    const int n = 3;
    return ((b - a + n) % n == 1) ? TransitionKind::SlideLeft
                                  : TransitionKind::SlideRight;
  }
  return TransitionKind::Fade;
}

void ScreenManager::set_screen(Screen s) {
  from_ = to_ = s;
  kind_ = TransitionKind::None;
  elapsed_ = 0.0f;
}

void ScreenManager::go_to(Screen s) { go_to(s, kind_for(to_, s)); }

void ScreenManager::go_to(Screen s, TransitionKind k, float dur_s) {
  if (s == to_ && k != TransitionKind::Dissolve) return;
  from_ = to_;
  to_ = s;
  kind_ = k;
  dur_ = dur_s > 0.0f ? dur_s : kTransitionSeconds;
  elapsed_ = 0.0f;
  captured_ = false;
  to_anim_ = ScreenAnim{};  // the arriving screen starts its animation fresh
}

void ScreenManager::restart_with(TransitionKind k, float dur_s) {
  from_ = to_;
  kind_ = k;
  dur_ = dur_s;
  elapsed_ = 0.0f;
  captured_ = false;
  from_anim_ = to_anim_;   // the outgoing copy inherits where the screen was
  to_anim_ = ScreenAnim{};
}

void ScreenManager::render(Framebuffer& out, const UiState& ui, const Anim& a) {
  if (kind_ == TransitionKind::None) {
    draw_screen(out, to_, ui, a, to_anim_);
    return;
  }
  if (!captured_) {
    from_ui_ = ui;
    captured_ = true;
  }
  elapsed_ += a.dt;
  const float p = progress();

  // Both sides get the live clock, so neither freezes while the other moves.
  // The outgoing side keeps the state it had when the transition started, so a
  // status change dissolves from the old word into the new one.
  draw_screen(from_fb_, from_, from_ui_, a, from_anim_);
  draw_screen(to_fb_, to_, ui, a, to_anim_);
  compose(out, from_fb_, to_fb_, kind_, p);

  if (p >= 1.0f) {
    kind_ = TransitionKind::None;
    from_ = to_;
    from_anim_ = to_anim_;
  }
}

void compose(Framebuffer& out, const Framebuffer& from, const Framebuffer& to,
             TransitionKind k, float progress) {
  const float p = clamp01(progress);
  out.clear();

  switch (k) {
    case TransitionKind::SlideLeft:
    case TransitionKind::SlideRight: {
      const float dir = (k == TransitionKind::SlideLeft) ? -1.0f : 1.0f;
      const float e = ease::out_cubic(p);
      const float dx = e * kWidth * dir;
      blit_offset(out, from, dx, 0.0f);
      blit_offset(out, to, dx - dir * kWidth, 0.0f);
      break;
    }

    case TransitionKind::WipeUp:
    case TransitionKind::WipeDown: {
      // A push, not a reveal: the old screen leaves as the new one arrives.
      const float dir = (k == TransitionKind::WipeUp) ? -1.0f : 1.0f;
      const float e = ease::out_back(p);
      const float dy = e * kHeight * dir;
      blit_offset(out, from, 0.0f, dy);
      blit_offset(out, to, 0.0f, dy - dir * kHeight);
      break;
    }

    case TransitionKind::Fade: {
      // Through black, which reads as a deliberate pause rather than a smear.
      const float e = ease::in_out_sine(p);
      if (e < 0.5f) {
        const uint8_t k8 = static_cast<uint8_t>((1.0f - e * 2.0f) * 255.0f);
        for (int y = 0; y < kHeight; ++y)
          for (int x = 0; x < kWidth; ++x) out.set(x, y, from.get(x, y).scaled(k8));
      } else {
        const uint8_t k8 = static_cast<uint8_t>((e * 2.0f - 1.0f) * 255.0f);
        for (int y = 0; y < kHeight; ++y)
          for (int x = 0; x < kWidth; ++x) out.set(x, y, to.get(x, y).scaled(k8));
      }
      break;
    }

    case TransitionKind::Dissolve: {
      // Linear, deliberately: easing the density reads as a flicker.
      const int switched = static_cast<int>(p * kNumLeds + 0.5f);
      for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
          const int rank = dissolve_rank(x, y);
          if (rank < switched) {
            out.set(x, y, to.get(x, y));
            // The pixels crossing over this frame spark, which is what gives a
            // status change its snap. Not on the final frame: the transition
            // has to settle exactly on the destination, not a brightened copy.
            if (p < 1.0f && rank >= switched - 6) {
              out.add_scaled(x, y, to.get(x, y), 0.6f);
            }
          } else {
            out.set(x, y, from.get(x, y));
          }
        }
      }
      break;
    }

    case TransitionKind::None:
    default:
      for (int y = 0; y < kHeight; ++y)
        for (int x = 0; x < kWidth; ++x) out.set(x, y, to.get(x, y));
      break;
  }
}

}  // namespace panel
