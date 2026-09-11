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
  // The panel as a radial strip of a record. Content swings through rather
  // than sliding: see blit_disk. This is the one for moving between items in
  // a list, because a list on a disk is what the gesture implies.
  DiskUp,
  DiskDown,
  // A full-panel event, not a way of getting somewhere. A front of the new
  // colour bursts out of the icon, floods the whole panel, and is then drawn
  // back into the icon, leaving the new screen behind it. Two stages, so it
  // reads as the panel being claimed by something and then absorbing it,
  // rather than as one picture being replaced by another.
  Ignite,
  Count,
};

const char* transition_name(TransitionKind k);

// How long a kind should take when nobody says otherwise.
float transition_seconds(TransitionKind k);

constexpr float kTransitionSeconds = 0.25f;
// A disk has mass. It takes longer to come round, and it settles.
constexpr float kDiskSeconds = 0.42f;

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

// Where Ignite's front starts: the middle of the 8x8 icon.
constexpr float kIgniteOriginX = 3.5f;
constexpr float kIgniteOriginY = 3.5f;
// The far corner is 20.3 away under the row stretch below, so this is just
// enough to clear it. Any more and the front spends the difference travelling
// over ground it has already covered, which shows up as a dead plateau of flat
// colour in the middle of the event.
constexpr float kIgniteMaxRadius = 21.5f;
// Where the front stops growing and starts being drawn back in.
constexpr float kIgniteTurnPoint = 0.34f;
constexpr float kIgniteSeconds = 0.80f;

// Composites two frames. Exposed so the blends can be tested on their own.
// `accent` is the colour Ignite floods with; the other kinds ignore it.
void compose(Framebuffer& out, const Framebuffer& from, const Framebuffer& to,
             TransitionKind k, float progress, RGB accent = RGB(255, 255, 255));

}  // namespace panel
