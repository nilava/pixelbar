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

// How deep navigation can go. Home, then a group, then a setting, plus slack.
constexpr int kNavDepth = 4;

// The boot screen runs for as long as its sequence takes. panel::kBootSeconds
// owns that number, because the animation is what decides when it has finished
// saying what it had to say.

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

  // Set directly rather than by gesture, for the web page. A status arriving
  // from elsewhere is still an event on the panel, so it plays the same claim
  // animation a finger would have caused.
  void set_status_external(int status);
  void set_brightness_external(int v);

  void render(panel::Framebuffer& fb, const panel::Anim& a);

  const panel::UiState& state() const { return ui_; }
  const Settings& settings() const { return set_; }
  // What the render path should be allowed to draw. Passed through rather than
  // read from a constant so the bench supply and the finished one both work.
  float max_ma() const { return static_cast<float>(set_.max_ma); }
  panel::Screen screen() const { return nav_[depth_ - 1].screen; }
  int depth() const { return depth_; }
  bool asleep() const { return asleep_; }
  // How long the address stays up after setup succeeds. Long enough to read a
  // dotted quad off a scrolling panel and type it in, short enough that the
  // device does not sit on a notice forever.
  static constexpr float kNetInfoSeconds = 12.0f;

  // The screen predicate, for tests: whether the panel is on one of the
  // network notices rather than a view the user chose.
  static bool is_net_screen_public(panel::Screen s) { return is_net_screen(s); }

  bool busy() const { return mgr_.busy(); }
  float transition_progress() const { return mgr_.progress(); }
  // The screen a transition is leaving. Exposed so a test can tell a real
  // movement from one that is blending a screen against itself.
  panel::Screen transition_from() const { return mgr_.previous(); }
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
  void goto_view(int index, int dir);
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
  // Shown, not navigated to: these are states the device is in. The nav stack
  // is reset to depth one so that a press or a turn leaves normally rather
  // than popping back into a screen the user never chose.
  void show_net(panel::Screen s);
  // True for the three network notices, which share a way in and a way out.
  static bool is_net_screen(panel::Screen s) {
    return s == panel::Screen::WifiSetup || s == panel::Screen::WifiConnecting ||
           s == panel::Screen::WifiInfo;
  }
  // Whether the person in front of the panel is following a setup they
  // started. Set when setup mode begins and cleared once an address arrives,
  // so the boot-time join stays silent while the one they just triggered from
  // the phone reports itself.
  bool net_watching_ = false;
  Ports::NetMode net_mode_ = Ports::NetMode::Online;
  float net_info_s_ = 0.0f;

  int view_ = 0;  // index into kHomeViews

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
