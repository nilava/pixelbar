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
    case TransitionKind::DiskUp: return "diskup";
    case TransitionKind::DiskDown: return "diskdown";
    case TransitionKind::Ignite: return "ignite";
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
    // The views are items on a record, not cards in a stack. Wrapping from the
    // last back to the first should still read forward.
    const int n = 3;
    return ((b - a + n) % n == 1) ? TransitionKind::DiskUp
                                  : TransitionKind::DiskDown;
  }
  return TransitionKind::Fade;
}

float transition_seconds(TransitionKind k) {
  if (k == TransitionKind::DiskUp || k == TransitionKind::DiskDown) return kDiskSeconds;
  if (k == TransitionKind::Ignite) return kIgniteSeconds;
  return kTransitionSeconds;
}

void ScreenManager::set_screen(Screen s) {
  from_ = to_ = s;
  kind_ = TransitionKind::None;
  elapsed_ = 0.0f;
}

void ScreenManager::go_to(Screen s, const UiState& leaving) {
  const TransitionKind k = kind_for(to_, s);
  go_to(s, leaving, k, transition_seconds(k));
}

void ScreenManager::go_to(Screen s, const UiState& leaving, TransitionKind k,
                          float dur_s) {
  if (s == to_ && k != TransitionKind::Dissolve) return;
  from_ = to_;
  to_ = s;
  kind_ = k;
  dur_ = dur_s > 0.0f ? dur_s : kTransitionSeconds;
  elapsed_ = 0.0f;
  from_ui_ = leaving;
  to_anim_ = ScreenAnim{};  // the arriving screen starts its animation fresh
}

void ScreenManager::restart_with(const UiState& leaving, TransitionKind k,
                                 float dur_s) {
  from_ = to_;
  kind_ = k;
  dur_ = dur_s;
  elapsed_ = 0.0f;
  from_ui_ = leaving;
  from_anim_ = to_anim_;   // the outgoing copy inherits where the screen was
  to_anim_ = ScreenAnim{};
}

void ScreenManager::render(Framebuffer& out, const UiState& ui, const Anim& a) {
  if (kind_ == TransitionKind::None) {
    draw_screen(out, to_, ui, a, to_anim_);
    return;
  }
  elapsed_ += a.dt;
  const float p = progress();

  // Both sides get the live clock, so neither freezes while the other moves.
  // The outgoing side keeps the state it had when the transition started, so a
  // status change dissolves from the old word into the new one.
  draw_screen(from_fb_, from_, from_ui_, a, from_anim_);
  draw_screen(to_fb_, to_, ui, a, to_anim_);
  compose(out, from_fb_, to_fb_, kind_, p, status_color(ui.status));

  if (p >= 1.0f) {
    kind_ = TransitionKind::None;
    from_ = to_;
    from_anim_ = to_anim_;
  }
}

void compose(Framebuffer& out, const Framebuffer& from, const Framebuffer& to,
             TransitionKind k, float progress, RGB accent) {
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

    case TransitionKind::DiskUp:
    case TransitionKind::DiskDown: {
      // The outgoing item carries on round and the incoming one arrives from
      // the opposite side, both sheared by radius, so the two are always the
      // same angle apart. The rim ends cross fast, the hub ends barely part.
      //
      // out_back rather than out_cubic: a disk with mass overshoots its detent
      // and comes back, and the small recoil is most of what makes it feel
      // like something you turned rather than something that was redrawn.
      const float dir = (k == TransitionKind::DiskUp) ? -1.0f : 1.0f;
      const float e = ease::out_back(p);
      const float turn = e * kDiskClearTurn * dir;

      // The hub end cannot travel far enough to leave the panel on its own —
      // that is inherent to a disk, not a shortcut — so it is faded out. The
      // curves are cubic at the ends and flat in the middle on purpose: both
      // items stay near full brightness while they are actually sweeping past
      // each other, so what you see is two things moving, and the fade only
      // does its work in the moments either side. Make these linear and the
      // whole thing collapses back into a cross-fade with extra steps.
      // The rim leaves and arrives by moving, so it keeps its brightness until
      // the very end (the cube). The hub cannot move, so it crosses over early
      // (the near-linear curve) and is finished with before the eye gets to it.
      const float q = 1.0f - p;
      const float hub = std::pow(p, 0.8f);
      blit_disk(out, from, turn, 1.0f - hub, 1.0f - p * p * p);
      blit_disk(out, to, turn - dir * kDiskClearTurn, hub, 1.0f - q * q * q);
      break;
    }

    case TransitionKind::Ignite: {
      // Stage one grows the front out of the icon and floods behind it. Stage
      // two draws the front back in, and what it uncovers is the new screen.
      // The field is the same object throughout, which is what makes this read
      // as one event rather than two effects played back to back.
      const bool drawing_in = p >= kIgniteTurnPoint;
      float r;
      if (!drawing_in) {
        // Out fast: this is a release of something, not a slide.
        r = ease::out_cubic(p / kIgniteTurnPoint) * kIgniteMaxRadius;
      } else {
        // Drawn back in quickly at first and then slowly, so the flood is gone
        // almost at once but the last of it lingers over the icon and settles
        // there. An in_out curve here instead leaves the panel flat and full
        // for a third of the event while the drain gets going.
        const float q = (p - kIgniteTurnPoint) / (1.0f - kIgniteTurnPoint);
        r = (1.0f - ease::out_cubic(q)) * kIgniteMaxRadius;
      }

      // The flood dims as it is drawn back in, so the last thing to leave is
      // the icon rather than a bright disc sitting on top of the new screen.
      //
      // A full-panel flood is the highest-power frame the UI ever draws: 192
      // LEDs of saturated colour. The cap in Renderer::render already bounds
      // it, so at high brightness this visibly dims rather than browning out
      // the supply. That is the cap doing its job, not a bug in the flourish.
      const float field_k = drawing_in ? 0.35f + 0.65f * (r / kIgniteMaxRadius) : 1.0f;
      const RGB field = accent.scaled(static_cast<uint8_t>(field_k * 255.0f + 0.5f));
      const RGB rim = lerp_rgb(accent, RGB(255, 255, 255), 0.75f);

      for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
          const float dx = static_cast<float>(x) - kIgniteOriginX;
          const float dy = (static_cast<float>(y) - kIgniteOriginY) * 1.6f;
          // The rows are stretched because eight of them against twenty-four
          // columns would otherwise make the front an ellipse that reaches the
          // top and bottom edges almost at once, and the expansion would read
          // as a vertical wipe instead of something circular.
          const float d = std::sqrt(dx * dx + dy * dy);

          if (d < r - 0.5f) {
            out.set(x, y, field);
          } else {
            // Ahead of the front: the old screen on the way out, the new one
            // on the way back in.
            const RGB under = drawing_in ? to.get(x, y) : from.get(x, y);
            if (drawing_in) {
              out.set(x, y, under);
            } else {
              // Dim what the front has not reached yet, so the panel is not
              // brightest where nothing is happening.
              const float k = 1.0f - 0.55f * ease::out_cubic(p / kIgniteTurnPoint);
              out.set(x, y, under.scaled(static_cast<uint8_t>(k * 255.0f + 0.5f)));
            }
          }

          // The front itself, a soft bright shell one pixel or so thick.
          // Not drawn at zero radius: at the two endpoints the composite has to
          // be exactly the old or the new screen, with nothing added on top.
          const float e = d - r;
          if (r > 0.05f && e > -1.4f && e < 0.9f) {
            const float w = 1.0f - std::fabs(e + 0.25f) / 1.15f;
            if (w > 0.0f) out.add_scaled(x, y, rim, w * w);
          }
        }
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
