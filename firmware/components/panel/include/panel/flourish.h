// Full-panel moments, and the small ones that sit over a screen.
//
// A transition gets you from one screen to another and should be over before
// you have finished noticing it. A flourish is the event itself — the timer
// finishing, a value you are dragging — and is allowed to take the panel and a
// second of your attention, because that is the whole point of the device.
//
// Flourishes are not on the navigation stack. They have their own clock, they
// draw over whatever is underneath, and when they finish the panel is exactly
// where it was. Nothing has to be pushed or popped, which is what stops a
// notification arriving mid-menu from losing your place.
#pragma once
#include <cstdint>

#include "panel/anim.h"
#include "panel/color.h"
#include "panel/framebuffer.h"

namespace panel {

struct Icon;

enum class FlourishKind : uint8_t {
  None,
  Done,   // the whole panel: a flash, a colour field, a word held, a fade
  Toast,  // a brief icon and word over the current screen
  Bar,    // a value being dragged, over the current screen
  Count,
};

// The Done timeline, in seconds from the start. Taken from frame-stepping the
// real thing: a flash of one or two frames, a quarter-second settle into the
// brand colour, about a second held, and a quarter-second fade.
constexpr float kDoneFlashS = 0.06f;
constexpr float kDoneSettleS = 0.30f;
constexpr float kDoneHoldS = 1.25f;
constexpr float kDoneSeconds = 1.60f;

constexpr float kToastSeconds = 0.90f;
// Long enough to read after the knob stops, short enough not to sit in the way.
constexpr float kBarSeconds = 1.00f;

// The Done flash is the brightest frame the UI ever draws: 192 LEDs of white,
// which at full value asks for well over 5 A. This is what fits the 2500 mA
// budget, and it is the honest ceiling rather than a taste decision — the cap
// would otherwise scale this one frame and nothing else, turning a deliberate
// flash into a shrug.
//
// It still reads as a flash because every LED is lit where a normal screen
// lights about a quarter of them: the total light is several times higher even
// though each LED is dimmer.
constexpr float kFlashLevel = 0.50f;

class Flourish {
 public:
  // The timer finished, the thing completed. Takes the panel.
  void done(RGB color, const char* word, double now_s);
  // A brief confirmation over whatever is showing.
  void toast(RGB color, const char* word, const Icon* icon, double now_s);
  // A value being dragged. Re-arming an already-showing bar just refreshes it,
  // so turning a knob steadily does not restart the animation on every detent.
  void bar(RGB color, float value, double now_s);

  void cancel() { kind_ = FlourishKind::None; }

  // Expires it on time rather than on being drawn.
  //
  // Without this, kind() keeps reporting a flourish that finished seconds ago
  // to anything that does not render — which is everything in the model. The
  // clock is what a flourish lives by, so the clock is what has to end it.
  void tick(double now_s) {
    if (kind_ != FlourishKind::None && !active(now_s)) kind_ = FlourishKind::None;
  }

  FlourishKind kind() const { return kind_; }
  bool active(double now_s) const;
  // True while it covers the panel completely, so the caller can skip drawing
  // the screen underneath and save the work.
  bool opaque(double now_s) const;

  // Draws over whatever is already in fb.
  void draw(Framebuffer& fb, const Anim& a);

  float seconds() const;

 private:
  FlourishKind kind_ = FlourishKind::None;
  RGB color_{255, 255, 255};
  char word_[8] = {0};
  const Icon* icon_ = nullptr;
  float value_ = 0.0f;
  double t0_ = 0.0;
};

}  // namespace panel
