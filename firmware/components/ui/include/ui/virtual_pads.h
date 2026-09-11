// Touch pads driven by something other than a finger.
//
// The simulator has a keyboard and the web page has buttons, and neither has a
// key-up: you get "the user tapped zone 1", not a rising edge followed 60 ms
// later by a falling one. This turns the first into the second.
//
// It matters that it does. A tap that arrives as an *event* skips the gesture
// recogniser entirely, so the double-tap window, the chord rule and the swipe
// discrimination are all bypassed — and the thing you are testing from the web
// page stops being the thing that will run when the pads are soldered on. Here
// a virtual tap is a pad level going high and coming back down over several
// frames, exactly like a real one, and everything downstream is none the wiser.
#pragma once
#include <cstdint>

#include "ui/event.h"

namespace ui {

// How long a requested tap holds the pad down. Comfortably past the debounce
// and past nothing else: long enough to register, short enough that two in
// quick succession still land inside the double-tap window.
constexpr float kVirtualTapSeconds = 0.06f;

class VirtualPads {
 public:
  // A tap: down now, up shortly.
  void tap(int zone);
  // Held until released. Used for anything that needs a hold or a chord.
  void set_held(int zone, bool held);
  void release_all();

  // A drag across all three pads, with the overlaps a finger makes. +1 runs
  // left to right.
  void swipe(int dir);

  // Advances the schedule by one frame. Call once per frame, before read().
  void advance(float dt_s);
  // ORs the virtual levels into whatever the real pins are reporting, so a
  // soldered pad and a web button are the same thing to everything above.
  void apply(bool* touch, int count) const;

  bool any_down() const;

 private:
  bool held_[kZones] = {false, false, false};
  float tap_left_[kZones] = {0.0f, 0.0f, 0.0f};
  // The drag is a small script: a step index and the time left on it.
  int8_t drag_step_ = -1;
  int8_t drag_dir_ = 1;
  float drag_t_ = 0.0f;
};

}  // namespace ui
