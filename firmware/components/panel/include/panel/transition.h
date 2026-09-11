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
//
// Long enough to be seen, now that a detent arriving mid-turn retargets it
// rather than starting it again.
constexpr float kDiskSeconds = 0.50f;

// Deterministic dissolve order. 89 is coprime with 192, so this is a bijection
// over the panel: every pixel gets a distinct rank and the dissolve can be
// tested rather than eyeballed.
constexpr int kDissolveStep = 89;
constexpr int dissolve_rank(int x, int y) {
  return ((y * kWidth + x) * kDissolveStep) % kNumLeds;
}

class ScreenManager {
 public:
  void set_screen(Screen s);  // no transition

  // Every one of these takes the state the panel is leaving.
  //
  // The outgoing screen has to be drawn with the state it had when the move
  // began, or a status change shows the new word on both sides of its own
  // transition. Snapshotting lazily on the first transition frame does not
  // work, because by then the caller has already written the new value into
  // the UiState it is about to pass to render(). So the snapshot is taken
  // here, from an argument, at the moment the caller still holds the old one.
  void go_to(Screen s, const UiState& leaving);  // picks the kind semantically
  void go_to(Screen s, const UiState& leaving, TransitionKind k,
             float dur_s = kTransitionSeconds);

  // Re-runs the current screen, for a change in place such as a new status.
  void restart_with(const UiState& leaving, TransitionKind k,
                    float dur_s = kTransitionSeconds);

  static TransitionKind kind_for(Screen from, Screen to);

  // Advancing and drawing are separate because time has to move in one place.
  //
  // When render() also advanced the clock, a transition only progressed if
  // something drew it — so a model that updated faster than the display, or
  // that asked whether it was still busy without rendering, got a frozen
  // transition and a busy() that never went false. advance() belongs with the
  // rest of the model's tick; render() is a pure function of where it got to.
  // Hand a transition already in flight a new destination.
  //
  // The first version of this kept the same movement running and only swapped
  // where it landed, on the reasoning that a detent arrives faster than the
  // disk plays and restarting on each one would never let the shear appear.
  // That reasoning was right and the implementation of it was wrong in two
  // ways that only showed up on a knob being spun hard.
  //
  // It left `from_` alone. So a fast turn blended the *original* screen
  // against a destination that kept changing, every intermediate view went
  // unseen, and the clock ran out mid-sweep — which is why the animation
  // appeared to be skipped entirely rather than shortened. Worse, turning
  // forward and straight back set the destination to the screen we were
  // already leaving, so the panel spent the rest of the transition blending a
  // screen against itself: a visible dead stop.
  //
  // The honest model is that each detent is its own short leg. What was
  // arriving becomes what is leaving, the new screen arrives, and the clock
  // starts over. Spun fast you see the first third of each leg, which is
  // exactly what a detented carousel does; turned back you get a real
  // transition in the other direction instead of a stall. `k` is passed in
  // because direction belongs to the gesture, not to the transition already
  // running.
  void retarget(Screen s, const UiState& leaving, TransitionKind k,
                float seconds);

  void advance(float dt_s);
  void render(Framebuffer& out, const UiState& ui, const Anim& a);

  bool busy() const { return kind_ != TransitionKind::None; }
  Screen current() const { return to_; }
  Screen previous() const { return from_; }
  float progress() const { return elapsed_ >= dur_ ? 1.0f : elapsed_ / dur_; }

 private:
  Framebuffer from_fb_, to_fb_;
  ScreenAnim from_anim_, to_anim_;
  // The state the outgoing screen was showing when the transition began. A
  // status change has to animate from the old word to the new one, so the
  // departing screen cannot be redrawn with the new data.
  UiState from_ui_;
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
