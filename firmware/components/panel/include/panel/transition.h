// Screen transitions.
//
// The motion is semantic: moving through the view cycle pushes sideways,
// opening an adjust screen wipes up like a drawer, a status change dissolves
// pixel by pixel, and sleep fades through black. The gesture teaches the model.
//
// Both screens keep animating for the whole transition. They are drawn into
// separate framebuffers with separate animation state every frame, then
// composited, so neither one freezes mid-move.
#pragma once
#include <cstdint>

#include "panel/anim.h"
#include "panel/framebuffer.h"
#include "panel/screens.h"

namespace panel {

enum class TransitionKind : uint8_t {
  None,
  SlideLeft,
  SlideRight,
  WipeUp,
  WipeDown,
  Dissolve,
  Fade,
  Count,
};

const char* transition_name(TransitionKind k);

constexpr float kTransitionSeconds = 0.25f;

// Deterministic dissolve order. 89 is coprime with 192, so this is a bijection
// over the panel: every pixel gets a distinct rank and the dissolve can be
// tested rather than eyeballed.
constexpr int kDissolveStep = 89;
constexpr int dissolve_rank(int x, int y) {
  return ((y * kWidth + x) * kDissolveStep) % kNumLeds;
}

class ScreenManager {
 public:
  void set_screen(Screen s);                 // no transition
  void go_to(Screen s);                      // picks the kind semantically
  void go_to(Screen s, TransitionKind k, float dur_s = kTransitionSeconds);

  // Re-runs the current screen with a dissolve, for a status change in place.
  void restart_with(TransitionKind k, float dur_s = kTransitionSeconds);

  static TransitionKind kind_for(Screen from, Screen to);

  void render(Framebuffer& out, const UiState& ui, const Anim& a);

  bool busy() const { return kind_ != TransitionKind::None; }
  Screen current() const { return to_; }
  Screen previous() const { return from_; }
  float progress() const { return elapsed_ >= dur_ ? 1.0f : elapsed_ / dur_; }

 private:
  Framebuffer from_fb_, to_fb_;
  ScreenAnim from_anim_, to_anim_;
  // The state the outgoing screen was showing when the transition began. A
  // status change has to dissolve from the old word to the new one, so the
  // departing screen cannot be redrawn with the new data.
  UiState from_ui_;
  bool captured_ = false;
  Screen from_ = Screen::Booting;
  Screen to_ = Screen::Booting;
  TransitionKind kind_ = TransitionKind::None;
  float elapsed_ = 0.0f;
  float dur_ = kTransitionSeconds;
};

// Composites two frames. Exposed so the blends can be tested on their own.
void compose(Framebuffer& out, const Framebuffer& from, const Framebuffer& to,
             TransitionKind k, float progress);

}  // namespace panel
