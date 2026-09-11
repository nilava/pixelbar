// The model: what the device is doing, and what every gesture does to it.
//
// App owns the state, the screen stack and the recogniser, so both the firmware
// and the simulator reduce to the same three lines a frame:
//
//     app.update(dt, now);
//     app.render(fb, anim);
//     renderer.render(fb, wire, app.state().brightness, ...);
//
// It is free of ESP-IDF, so the whole navigation model is driven from tests and
// from a terminal before any of it meets a finger.
#pragma once
#include <cstdint>

#include "panel/flourish.h"
#include "panel/screens.h"
#include "panel/transition.h"
#include "ui/gesture.h"
#include "ui/ports.h"

namespace ui {

// The three views the knob cycles at rest. Scene joins them once the pattern
// engine is folded in.
constexpr panel::Screen kHomeViews[] = {panel::Screen::Status, panel::Screen::Clock,
                                        panel::Screen::Timer};
constexpr int kHomeViewCount = 3;

// The adjust screens, stepped through with the knob.
constexpr panel::Screen kAdjustViews[] = {
    panel::Screen::Brightness, panel::Screen::ColorPick, panel::Screen::TimerSet};
constexpr int kAdjustViewCount = 3;

// How deep navigation can go. Home, then a group, then a setting, plus slack.
constexpr int kNavDepth = 4;

// How long the boot screen holds before the panel settles on its home view.
// Short, because it is showing you that the device woke up, not asking you to
// read anything.
constexpr float kBootSeconds = 1.5f;

// The transition that takes you back out of wherever a kind took you in.
panel::TransitionKind inverse_of(panel::TransitionKind k);

struct NavFrame {
  panel::Screen screen = panel::Screen::Status;
  // Recorded so that going back plays the inverse. kind_for() is a pure
  // function of the screen pair, so it cannot tell a push from a pop; without
  // this, backing out of a drawer would wipe in the same direction it opened.
  panel::TransitionKind enter_kind = panel::TransitionKind::None;
};

class App {
 public:
  void begin(Ports& ports, double now_s);

  // Reads input, applies it, advances timers. dt is seconds.
  void update(float dt_s, double now_s);

  // Applies one event directly. update() calls this for everything the
  // recogniser produced; tests and the simulator can also call it.
  void handle(const Event& e, double now_s);

  void render(panel::Framebuffer& fb, const panel::Anim& a);

  const panel::UiState& state() const { return ui_; }
  const Settings& settings() const { return set_; }
  panel::Screen screen() const { return nav_[depth_ - 1].screen; }
  int depth() const { return depth_; }
  bool asleep() const { return asleep_; }
  bool busy() const { return mgr_.busy(); }
  panel::FlourishKind flourish() const { return fl_.kind(); }

  // The last event the model acted on, for the simulator's status line.
  EventType last_event() const { return last_; }

  // Settings are written back through the port when they settle rather than on
  // every detent, so spinning the brightness knob is not a hundred flash
  // writes. Exposed so a test can assert the debounce rather than infer it.
  bool save_pending() const { return save_pending_; }

 private:
  void push(panel::Screen s, panel::TransitionKind k);
  void enter_menu_entry();
  void pop();
  void go_home();
  void goto_view(int index);
  void set_status(panel::Status s);
  void wake();
  void sleep();
  void adjust(int detents, float rate);
  void note_change();  // a setting moved: start the save debounce

  Ports* ports_ = nullptr;
  Recogniser rec_;
  Settings set_;
  panel::UiState ui_;
  panel::ScreenManager mgr_;
  // Not on the nav stack: a flourish draws over whatever is there and leaves
  // the panel exactly where it was, so a timer finishing mid-menu does not
  // lose your place.
  panel::Flourish fl_;

  NavFrame nav_[kNavDepth];
  int depth_ = 1;
  int view_ = 0;    // index into kHomeViews
  int adjust_ = 0;  // index into kAdjustViews

  bool asleep_ = false;
  bool booting_ = true;
  float boot_s_ = 0.0f;
  float idle_s_ = 0.0f;
  float timer_accum_s_ = 0.0f;
  float save_after_s_ = 0.0f;
  bool save_pending_ = false;
  EventType last_ = EventType::None;
  double now_s_ = 0.0;
};

}  // namespace ui
