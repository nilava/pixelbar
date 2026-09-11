// What the panel can be told to do, and the raw state it is worked out from.
//
// This header and everything beside it are free of ESP-IDF, for the same
// reason the drawing layer is: the interesting logic is the part worth testing
// on a laptop, and a gesture recogniser is nothing but edge cases. The device
// contributes raw pin levels and an accumulated encoder count; every judgement
// about what those *mean* happens here, where a test can drive it.
#pragma once
#include <cstdint>

namespace ui {

// The three touch pads under the top edge, left to right as you face the panel.
enum class Zone : uint8_t { Left, Middle, Right, Count };
constexpr int kZones = 3;

constexpr uint8_t zone_bit(Zone z) { return static_cast<uint8_t>(1u << static_cast<int>(z)); }

enum class EventType : uint8_t {
  None,
  Tap,             // which = zone
  DoubleTap,       // which = zone; always preceded by a Tap, and supersedes it
  HoldBegin,       // which = zone
  HoldEnd,         // which = zone
  Chord,           // which = a bitmask of the zones held together
  Swipe,           // delta = +1 left-to-right, -1 right-to-left
  Turn,            // delta = detents, velocity = detents per second
  Press,           // the encoder switch
  DoublePress,
  PressHoldBegin,
  PressHoldEnd,
  PressTurn,       // turned while the knob is held down
  CaseTap,         // the accelerometer felt a knock
  Shake,
  LaidFlat,
  PickedUp,
  Count,
};

const char* event_name(EventType t);

struct Event {
  EventType type = EventType::None;
  uint8_t which = 0;       // a zone index, or a zone bitmask for Chord
  int16_t delta = 0;
  float velocity = 0.0f;

  Event() = default;
  explicit Event(EventType t, uint8_t w = 0, int16_t d = 0, float v = 0.0f)
      : type(t), which(w), delta(d), velocity(v) {}
};

// Everything the recogniser is fed each frame.
//
// The encoder count is accumulated rather than a per-frame delta: the interrupt
// adds to it and the recogniser takes the difference, so a frame that runs late
// loses nothing. A per-frame delta would drop detents exactly when the knob is
// being spun hardest, which is when it matters.
struct RawInput {
  bool touch[kZones] = {false, false, false};
  bool encoder_sw = false;
  int32_t encoder_detents = 0;

  // Acceleration in g, panel axes. Only read when motion_valid is set, so the
  // whole motion layer is absent rather than wrong when there is no sensor.
  float ax = 0.0f, ay = 0.0f, az = 0.0f;
  bool motion_valid = false;
};

// Timings, gathered so they can be tuned in one place and driven from tests.
//
// These are human-factors numbers except where noted. They are worth arguing
// about in the simulator rather than on hardware; the four that genuinely need
// a real board are called out in the plan.
struct GestureConfig {
  // Past a slow tap, short of feeling stuck.
  float hold_s = 0.45f;
  // Below 250 ms people miss the second tap; above 350 ms every tap feels late.
  float double_tap_s = 0.28f;
  // Two pads inside this are one chord, not two taps.
  float chord_s = 0.12f;
  // Adjacent pads in a swipe, one to the next.
  float swipe_step_s = 0.25f;
  // The TTP223 debounces in hardware, so this only rejects a single stray
  // sample. Whether it is enough is the first thing to measure on a real board,
  // under a full-white frame at 2.5 A.
  float touch_stable_s = 0.003f;
  // Window the turn rate is averaged over, for the acceleration multiplier.
  float turn_window_s = 0.20f;

  // The encoder's push switch is a bare mechanical contact with nothing
  // debouncing it, unlike the TTP223s. It needs a longer window than they do —
  // and not only for its own bounce. On a KY-040 module the switch shares its
  // ground net with the rotary contacts, so turning the knob bounces that
  // ground and the switch line dips with it. Untreated, ordinary turning
  // registers as press-and-turn and the panel fights between changing screen
  // and changing brightness.
  float switch_stable_s = 0.015f;

  // How long after the knob last moved before the switch is believed again.
  //
  // A longer debounce was the first answer and it is the wrong one: during a
  // fast turn the rotary contacts make and break continuously, so the ground
  // they share with the switch is disturbed for as long as you keep turning,
  // not for a few milliseconds. No debounce window short enough to keep a press
  // feeling instant is long enough to outlast that.
  //
  // So the switch is simply not read while the knob is moving. That is not a
  // workaround, it is the physical fact: on this module the switch line is not
  // trustworthy while the rotary contacts are working. Nothing is lost, because
  // turning while pressed is not a gesture any more — and in practice you stop
  // turning before you press.
  float switch_settle_after_turn_s = 0.09f;

  // The shortest contact that counts as a press.
  //
  // The guard above only helps once a detent has been decoded, and a detent is
  // four quadrature edges — at a brisk turn that is some tens of milliseconds
  // after the knob actually started moving. The rotary contacts are already
  // chattering through that window and the shared ground is already dipping
  // the switch line, but as far as the recogniser is concerned the knob has
  // been still for seconds. So the dip latches as a press, and by the time the
  // turn is visible the press has already been and gone.
  //
  // No amount of looking backwards fixes that, because the turn has not
  // happened yet. What separates the two is duration: a finger holds the
  // button down for a tenth of a second at the very least, and a coupled dip
  // is over in a few milliseconds. Press is emitted on release, so requiring a
  // minimum dwell costs no latency at all — the time was already spent.
  //
  // Thirty milliseconds is deliberately well below the shortest click anyone
  // produces on purpose — a quick one is fifty, a normal one over eighty — so
  // this rejects bounce without ever making the button feel unreliable. It
  // does not need to be tight, only to be somewhere in the gap.
  float min_press_s = 0.03f;

  // Motion thresholds, in g. The tap figure is the least grounded number in
  // the design and must be measured against a desk bump before it is trusted.
  float tap_g = 1.2f;
  float shake_g = 1.8f;
  // Below this on the in-plane axis the case is lying down rather than standing.
  float flat_g = 0.4f;
  float flat_dwell_s = 1.0f;

  bool enable_swipe = true;
  bool enable_motion = true;
};

}  // namespace ui
