#include "ui/app.h"

#include "panel/anim.h"
#include "panel/config.h"

namespace ui {

using panel::Screen;
using panel::Status;
using panel::TransitionKind;

namespace {

// How long a setting has to sit still before it is written back. Long enough
// that a knob sweep is one write, short enough that you cannot beat it to the
// power switch.
constexpr float kSaveDebounceS = 1.5f;

// Above this many detents a second, a turn steps faster. Picked so a
// deliberate single click is never multiplied.
constexpr float kFastTurnRate = 8.0f;
constexpr int kFastTurnStep = 5;

int wrap_index(int i, int n) {
  while (i < 0) i += n;
  return i % n;
}

}  // namespace

panel::TransitionKind inverse_of(panel::TransitionKind k) {
  switch (k) {
    case TransitionKind::WipeUp: return TransitionKind::WipeDown;
    case TransitionKind::WipeDown: return TransitionKind::WipeUp;
    case TransitionKind::DiskUp: return TransitionKind::DiskDown;
    case TransitionKind::DiskDown: return TransitionKind::DiskUp;
    case TransitionKind::SlideLeft: return TransitionKind::SlideRight;
    case TransitionKind::SlideRight: return TransitionKind::SlideLeft;
    default: return k;  // a fade and an ignite look the same coming back
  }
}

void Settings::sanitise() {
  // Measured rather than picked. Below this, a large and rapidly growing share
  // of the panel sits at a PWM value of one to three, where WS2812B parts are
  // least well behaved: 27% of lit channel-frames at 24, 51% at 12, 64% at 8.
  // Above it the curve has flattened — 20% at 48 — so this is where lowering
  // the brightness stops buying dimness and starts buying instability.
  //
  // It is a mitigation and not a cure. The minimum non-zero output is 1 at
  // every brightness, because anti-aliased edges and dithered fades produce
  // ones by construction.
  if (brightness < panel::kMinBrightness) brightness = panel::kMinBrightness;
  if (hue < 0.0f || hue > 1.0f) hue = 0.08f;
  if (sleep_after_min > 720) sleep_after_min = 720;
  if (max_ma < panel::kMinMilliamps) max_ma = static_cast<uint16_t>(panel::kMinMilliamps);
  if (max_ma > panel::kMaxAllowedMilliamps)
    max_ma = static_cast<uint16_t>(panel::kMaxAllowedMilliamps);
  if (work_min < 1) work_min = 1;
  if (work_min > 99) work_min = 99;
  if (rest_min < 1) rest_min = 1;
  if (rest_min > 99) rest_min = 99;
  if (cycles < 1) cycles = 1;
  if (cycles > 99) cycles = 99;
}

bool Settings::migrate() {
  if (version == kVersion) {
    sanitise();
    return true;
  }
  // Nothing older exists yet. A blob from the future is refused rather than
  // reinterpreted, because the alternative is a downgrade silently reading
  // someone else's field layout.
  return false;
}

// ------------------------------------------------------------------- App

void App::begin(Ports& ports, double now_s) {
  ports_ = &ports;
  now_s_ = now_s;

  Settings loaded;
  if (ports_->load_settings(&loaded) && loaded.migrate()) set_ = loaded;
  set_.sanitise();

  // One place where the stored struct becomes what the panel draws, shared
  // with every later change, so a field added to one and not the other shows
  // up as a setting that does nothing.
  apply_settings();
  // The whole timer, at the start of a set, so that cycle and phase agree with
  // the length rather than being defaulted separately.
  reset_timer();

  depth_ = 1;
  view_ = 0;
  booting_ = true;
  boot_s_ = 0.0f;
  nav_[0] = NavFrame{Screen::Booting, TransitionKind::None};
  mgr_.set_screen(Screen::Booting);
}

void App::update(float dt_s, double now_s) {
  now_s_ = now_s;

  if (ports_) {
    RawInput raw;
    ports_->read_raw(&raw);
    if (set_.touch_locked)
      for (int i = 0; i < kZones; ++i) raw.touch[i] = false;
    if (!set_.motion_enabled) raw.motion_valid = false;

    Event ev[kMaxEventsPerFrame];
    const int n = rec_.update(raw, dt_s, ev, kMaxEventsPerFrame);
    for (int i = 0; i < n; ++i) handle(ev[i], now_s);

    int h = 0, m = 0, s = 0;
    if (ports_->wall_clock(&h, &m, &s)) {
      ui_.hour = h;
      ui_.minute = m;
      ui_.second = s;
      ui_.time_valid = true;
    }
    ui_.wifi_connected = ports_->wifi_connected();
    ui_.net_text = ports_->net_text();

    // An update takes the panel, from wherever you happen to be. It is not
    // pushed onto the nav stack and there is no way out of it: the only exits
    // are the reboot at the end and a failure, both of which end it from the
    // other side.
    ui_.ota = ports_->ota_progress();
    if (ui_.ota >= 0.0f && screen() != panel::Screen::OtaProgress) {
      show_net(panel::Screen::OtaProgress);
    } else if (ui_.ota < 0.0f && screen() == panel::Screen::OtaProgress) {
      go_home();  // the upload failed; put the device back where it was
    }

    now_unix_ = ports_->unix_time();
    take_draw(dt_s);

    // Pairing, the same way. It outranks everything except an update, because
    // a code nobody can read is a code nobody can use, and it is on screen for
    // about twenty seconds once in the life of a host.
    ui_.passkey = ports_->passkey();
    if (ui_.passkey != 0 && screen() != panel::Screen::Pairing &&
        screen() != panel::Screen::OtaProgress) {
      show_net(panel::Screen::Pairing);
    } else if (ui_.passkey == 0 && screen() == panel::Screen::Pairing) {
      go_home();
    }

    // A device with nothing stored cannot be set up from the panel — there is
    // no way to type a password into 24 pixels — so the only useful thing it
    // can do is say which network to join. It says so once, when setup mode
    // begins, and then gets out of the way: the screen is left like any other,
    // and nothing drags you back to it.
    const Ports::NetMode mode = ports_->net_mode();
    if (mode != net_mode_) {
      const Ports::NetMode was = net_mode_;
      net_mode_ = mode;
      if (mode == Ports::NetMode::Setup) net_watching_ = true;
      if (!booting_) {
        if (mode == Ports::NetMode::Setup) {
          show_net(panel::Screen::WifiSetup);
        } else if (mode == Ports::NetMode::Joining && net_watching_) {
          // Only for a join someone is standing there waiting on. The one at
          // boot is covered by the boot sequence, and interrupting that with a
          // progress screen every power-up would be noise: the panel would
          // announce the network before it had said anything about itself.
          show_net(panel::Screen::WifiConnecting);
        } else if (mode == Ports::NetMode::Online && net_watching_) {
          // Setup just succeeded. Show the address for a few seconds, because
          // it is the one thing you need next and the only place it is
          // written down.
          net_watching_ = false;
          show_net(panel::Screen::WifiInfo);
          net_info_s_ = kNetInfoSeconds;
        }
        (void)was;
      }
    }
    // The address screen is a notice, not a destination.
    if (net_info_s_ > 0.0f && screen() == panel::Screen::WifiInfo) {
      net_info_s_ -= dt_s;
      if (net_info_s_ <= 0.0f) go_home();
    }
  }

  mgr_.advance(dt_s);
  fl_.tick(now_s);

  // Leave the boot screen once it has been seen. A connected radio ends it
  // early, because at that point it has nothing left to tell you.
  if (booting_) {
    boot_s_ += dt_s;
    ui_.boot_t = boot_s_;
    if (boot_s_ >= panel::kBootSeconds) {
      booting_ = false;
      // Straight into setup if that is where the device is: handing over to
      // the status screen first would show a working device that is not.
      if (net_mode_ == Ports::NetMode::Setup) {
        show_net(panel::Screen::WifiSetup);
      } else {
        nav_[0] = NavFrame{kHomeViews[view_], TransitionKind::None};
        mgr_.go_to(kHomeViews[view_], ui_, TransitionKind::Fade,
                   panel::transition_seconds(TransitionKind::Fade));
      }
    }
  }

  // The focus timer. Accumulating dt rather than comparing against an absolute
  // stamp is what keeps the simulator's pause and time dilation honest: pause
  // the clock and the timer stops with it, instead of leaping when it resumes.
  if (ui_.timer_running && ui_.timer_left_s > 0) {
    timer_accum_s_ += dt_s;
    while (timer_accum_s_ >= 1.0f && ui_.timer_left_s > 0) {
      timer_accum_s_ -= 1.0f;
      --ui_.timer_left_s;
    }
    if (ui_.timer_left_s <= 0) end_timer_phase(now_s);
  }

  // Idle, and the sleep timeout.
  idle_s_ += dt_s;
  if (!asleep_ && set_.sleep_after_min > 0 &&
      idle_s_ >= set_.sleep_after_min * 60.0f) {
    sleep();
  }

  if (save_pending_) {
    save_after_s_ -= dt_s;
    if (save_after_s_ <= 0.0f) {
      save_pending_ = false;
      set_.brightness = ui_.brightness;
      set_.hue = ui_.hue;
      set_.work_min = static_cast<uint16_t>(ui_.timer_set_min);
      if (ports_) ports_->save_settings(set_);
    }
  }
}

void App::render(panel::Framebuffer& fb, const panel::Anim& a) {
  // Skip the screen underneath while the flourish covers the panel: it would
  // be drawn and then entirely painted over, and this is the one moment the
  // frame budget is under any real pressure.
  if (fl_.opaque(a.t)) {
    fb.clear();
  } else {
    mgr_.render(fb, ui_, a);
  }
  fl_.draw(fb, a);
}

void App::note_change() {
  save_pending_ = true;
  save_after_s_ = kSaveDebounceS;
}

void App::push(Screen s, TransitionKind k) {
  if (depth_ >= kNavDepth) return;
  // Navigating onto the screen you are already on has to acknowledge somehow,
  // or the gesture reads as not having registered. go_to() returns early in
  // that case by design, so an in-place restart is the honest answer.
  if (s == mgr_.current()) {
    mgr_.restart_with(ui_, TransitionKind::Ignite,
                      panel::transition_seconds(TransitionKind::Ignite));
  } else {
    mgr_.go_to(s, ui_, k, panel::transition_seconds(k));
  }
  nav_[depth_++] = NavFrame{s, k};
}

void App::pop() {
  if (depth_ <= 1) return;
  const TransitionKind back = inverse_of(nav_[depth_ - 1].enter_kind);
  --depth_;
  const Screen to = nav_[depth_ - 1].screen;
  // The list the panel draws has to follow the level being returned to.
  // Without this, backing out of a setting showed the group's items under the
  // groups' heading — one list on screen and the other in the model.
  if (to == Screen::Group) refresh_group();
  else if (to == Screen::Menu) refresh_menu();
  mgr_.go_to(to, ui_, back, panel::transition_seconds(back));
}

void App::go_home() {
  // Defined by where it lands, not by how deep it starts.
  //
  // It used to be `while (depth_ > 1)`, which made it a silent no-op at depth
  // one — and the network notices sit at depth one on a screen that is not a
  // home view, so pressing to dismiss the address screen did nothing at all.
  // Asking whether we are already home is the condition that was meant.
  TransitionKind back = TransitionKind::Fade;
  if (depth_ > 1) {
    back = inverse_of(nav_[depth_ - 1].enter_kind);
  } else if (nav_[0].enter_kind != TransitionKind::None) {
    back = inverse_of(nav_[0].enter_kind);
  }
  depth_ = 1;
  const panel::Screen home = kHomeViews[view_];
  nav_[0] = NavFrame{home, TransitionKind::None};
  if (mgr_.current() == home) return;
  mgr_.go_to(home, ui_, back, panel::transition_seconds(back));
}

// ---------------------------------------------------------- settings tree
//
// Three levels, and the draw layer only ever sees one list at a time. These
// four functions are what keeps ui_ agreeing with where the model thinks it
// is; every navigation calls exactly one of them, and forgetting to is how a
// list ends up showing the previous level's items.

void App::refresh_menu() {
  int n = kGroupCount;
  if (n > kMaxListItems) n = kMaxListItems;
  for (int i = 0; i < n; ++i) list_buf_[i] = kGroups[i].row;
  ui_.list = list_buf_;
  ui_.list_count = n;
  ui_.menu_index = static_cast<uint8_t>(group_index_);
}

void App::refresh_group() {
  const SettingGroup& g = kGroups[group_index_];
  int n = g.count;
  if (n > kMaxListItems) n = kMaxListItems;
  for (int i = 0; i < n; ++i) list_buf_[i] = g.items[i].row;
  ui_.list = list_buf_;
  ui_.list_count = n;
  ui_.menu_index = static_cast<uint8_t>(item_index_);
}

const SettingDesc* App::current_setting() const {
  if (group_index_ < 0 || group_index_ >= kGroupCount) return nullptr;
  const SettingGroup& g = kGroups[group_index_];
  if (item_index_ < 0 || item_index_ >= g.count) return nullptr;
  return &g.items[item_index_];
}

void App::refresh_setting() {
  const SettingDesc* d = current_setting();
  if (!d) return;
  ui_.set_icon = d->row.icon;
  ui_.set_label = d->row.label;
  ui_.set_tint = d->row.color;
  ui_.set_text = setting_text(set_, *d, set_text_, sizeof(set_text_));
  ui_.set_fraction = setting_fraction(set_, *d);
  ui_.set_on = setting_get(set_, d->id) != 0;
}

void App::apply_settings() {
  // The settings struct is the record; UiState is what the panel draws from.
  // Anything stored that the panel shows has to be copied across here, and a
  // field that is not is a setting that persists and does nothing — which is
  // exactly the defect this whole tree was built to clear.
  ui_.brightness = set_.brightness;
  ui_.hue = set_.hue;
  ui_.accent = panel::accent_from_hue(set_.hue);
  ui_.scene = set_.scene;
  ui_.timer_set_min = set_.work_min;
  ui_.timer_cycles = set_.cycles;
  // Retarget a timer that is not running, so the number you just dialled is
  // the number that runs. Conditional on the target having actually changed,
  // or adjusting the brightness would silently reset a paused countdown.
  //
  // Whichever phase is on screen, not always the work one: dialling the rest
  // length while resting should change the rest you are looking at, and using
  // the work length here would have quietly replaced it.
  const int want = (ui_.timer_resting ? set_.rest_min : set_.work_min) * 60;
  if (!ui_.timer_running && ui_.timer_total_s != want) {
    ui_.timer_total_s = want;
    ui_.timer_left_s = want;
    timer_accum_s_ = 0.0f;
  }
}

// Collect a host's draw request, and decide whether it gets the panel.
void App::take_draw(float dt_s) {
  DrawPayload next;
  if (ports_->take_draw(&next)) {
    // Accepted when it is at least as important as what is already up. Equal
    // priority from a different source replaces rather than stacks: two things
    // that both think they are worth fifty are, and the newer one is the one
    // somebody just asked for.
    const bool outranks = !drawing_ || next.priority >= draw_.priority;
    if (outranks) {
      draw_ = next;
      drawing_ = true;
      draw_left_s_ = draw_.ttl_s;
      refresh_draw();
      if (screen() != panel::Screen::Draw) show_net(panel::Screen::Draw);
    }
  }

  if (!drawing_) return;

  // A countdown keeps the payload alive: something counting down to a moment
  // should not vanish before it arrives, whatever ttl was asked for.
  const bool counting = draw_.until_unix > 0 && ui_.draw_seconds > 0;
  if (draw_.ttl_s > 0.0f && !counting) {
    draw_left_s_ -= dt_s;
    if (draw_left_s_ <= 0.0f) {
      drawing_ = false;
      ui_.draw_text = "";
      ui_.draw_icon = nullptr;
      ui_.draw_seconds = -1;
      ui_.draw_bar = -1.0f;
      if (screen() == panel::Screen::Draw) go_home();
      return;
    }
  }
  refresh_draw();
}

void App::refresh_draw() {
  ui_.draw_text = draw_.text;
  ui_.draw_icon = panel::icon_by_name(draw_.icon);
  ui_.draw_bar = draw_.bar;
  ui_.draw_tint = panel::RGB(static_cast<uint8_t>((draw_.tint >> 16) & 0xFF),
                             static_cast<uint8_t>((draw_.tint >> 8) & 0xFF),
                             static_cast<uint8_t>(draw_.tint & 0xFF));

  // The countdown is worked out here, from a deadline and the wall clock,
  // rather than sent as a number that would be stale the moment it arrived.
  // Without a clock there is nothing to count against, so it is not drawn —
  // the same rule the Clock screen follows.
  ui_.draw_seconds = -1;
  if (draw_.until_unix > 0 && ui_.time_valid) {
    const int64_t left = draw_.until_unix - now_unix_;
    ui_.draw_seconds = left > 0 ? static_cast<int>(left > 5999 ? 5999 : left) : 0;
  }
}

void App::show_net(panel::Screen s) {
  // Depth one, and the nav frame set to this screen, so that pressing or
  // turning leaves it the ordinary way. Pushing it instead would make "back"
  // return to a screen nobody asked for.
  depth_ = 1;
  const TransitionKind k = TransitionKind::Ignite;
  // The kind is recorded even though nothing is pushed, so that leaving plays
  // the inverse rather than a default fade.
  nav_[0] = NavFrame{s, k};
  mgr_.go_to(s, ui_, k, panel::transition_seconds(k));
}

void App::goto_view(int index, int dir) {
  const int next = wrap_index(index, kHomeViewCount);
  view_ = next;
  depth_ = 1;
  nav_[0] = NavFrame{kHomeViews[view_], TransitionKind::None};
  if (kHomeViews[view_] == mgr_.current()) return;

  // The direction is the direction you turned, not the shorter way round the
  // carousel. Deriving it from the index difference got it backwards as soon as
  // a single event carried more than one detent.
  const TransitionKind k = dir > 0 ? TransitionKind::DiskUp : TransitionKind::DiskDown;
  const float secs = panel::transition_seconds(k);
  // Mid-turn each detent is its own leg, handed off from the screen that was
  // arriving. See ScreenManager::retarget for why keeping one movement running
  // across detents looked like no movement at all.
  if (mgr_.busy()) {
    mgr_.retarget(kHomeViews[view_], ui_, k, secs);
    return;
  }
  mgr_.go_to(kHomeViews[view_], ui_, k, secs);
}

void App::enter_menu_entry() {
  // Two levels of the same gesture: pressing on the group list opens that
  // group, pressing on a setting opens it. Matching on an index rather than on
  // a label, as this did, because the tree is now data — an entry cannot point
  // at the wrong screen when the entry *is* the descriptor.
  if (screen() == Screen::Menu) {
    group_index_ = ui_.menu_index;
    if (group_index_ < 0 || group_index_ >= kGroupCount) group_index_ = 0;
    const SettingGroup& g = kGroups[group_index_];
    if (g.direct != Screen::Count) {
      if (g.direct == Screen::StatusPick) ui_.pick = ui_.status;
      push(g.direct, TransitionKind::WipeUp);
      return;
    }
    item_index_ = 0;
    refresh_group();
    push(Screen::Group, TransitionKind::WipeUp);
    return;
  }

  const SettingDesc* d = current_setting();
  if (!d) return;
  if (d->kind == SettingKind::Screen) {
    // Brightness and hue keep their own pickers: a value you are choosing by
    // eye deserves to be shown as the thing itself rather than as a number
    // describing it.
    if (d->screen == Screen::StatusPick) ui_.pick = ui_.status;
    push(d->screen, TransitionKind::WipeUp);
    return;
  }
  refresh_setting();
  push(Screen::Setting, TransitionKind::WipeUp);
}

// One phase of the focus cycle has run out. Work becomes rest, rest becomes the
// next round of work, and the last round of work ends the set.
//
// There is deliberately no rest after the final round: the set is over, and a
// rest you are not going to come back from is just a second countdown between
// you and being finished.
void App::end_timer_phase(double now_s) {
  timer_accum_s_ = 0.0f;

  if (!ui_.timer_resting) {
    if (ui_.timer_cycle >= ui_.timer_cycles) {
      ui_.timer_running = false;
      // The whole set. This is the moment the device exists for, so it takes
      // the panel rather than sitting in a corner of it.
      fl_.done(ui_.accent, "DONE", now_s);
      return;
    }
    ui_.timer_resting = true;
    ui_.timer_total_s = set_.rest_min * 60;
  } else {
    ui_.timer_resting = false;
    ++ui_.timer_cycle;
    ui_.timer_total_s = set_.work_min * 60;
  }

  ui_.timer_left_s = ui_.timer_total_s;
  // A boundary is worth noticing but is not worth the whole panel: it is
  // punctuation, not an ending. Toast has been implemented and tested since
  // the flourish layer was written and called by nothing until now.
  fl_.toast(ui_.timer_resting ? panel::RGB(60, 180, 255) : ui_.accent,
            ui_.timer_resting ? "REST" : "WORK",
            ui_.timer_resting ? &panel::kIconMoon : &panel::kIconHourglass, now_s);
  // Carrying on by itself is a setting because both answers are reasonable:
  // some people want the next round to start while they are still stretching,
  // and some want to decide.
  ui_.timer_running = set_.auto_next;
}

void App::reset_timer() {
  ui_.timer_running = false;
  ui_.timer_resting = false;
  ui_.timer_cycle = 1;
  ui_.timer_cycles = set_.cycles;
  ui_.timer_total_s = set_.work_min * 60;
  ui_.timer_left_s = ui_.timer_total_s;
  timer_accum_s_ = 0.0f;
}

void App::set_status(Status s) {
  if (ui_.status == s) return;
  // A status change is the event this device exists for, so it takes the whole
  // panel wherever you happen to be standing.
  mgr_.restart_with(ui_, TransitionKind::Ignite,
                    panel::transition_seconds(TransitionKind::Ignite));
  ui_.status = s;
}

void App::set_status_external(int status) {
  if (status < 0 || status >= static_cast<int>(Status::Count)) return;
  if (asleep_) wake();
  idle_s_ = 0.0f;
  set_status(static_cast<Status>(status));
}

void App::set_brightness_external(int v) {
  if (v < panel::kMinBrightness) v = panel::kMinBrightness;
  if (v > 255) v = 255;
  ui_.brightness = static_cast<uint8_t>(v);
  note_change();
}

void App::wake() {
  if (!asleep_) return;
  asleep_ = false;
  idle_s_ = 0.0f;
  depth_ = 1;
  nav_[0] = NavFrame{kHomeViews[view_], TransitionKind::None};
  mgr_.go_to(kHomeViews[view_], ui_, TransitionKind::Fade,
             panel::transition_seconds(TransitionKind::Fade));
}

void App::sleep() {
  if (asleep_) return;
  asleep_ = true;
  depth_ = 1;
  nav_[0] = NavFrame{Screen::Sleep, TransitionKind::None};
  mgr_.go_to(Screen::Sleep, ui_, TransitionKind::Fade,
             panel::transition_seconds(TransitionKind::Fade));
}

void App::adjust(int detents, float rate) {
  // Acceleration applies to *values* and never to *lists*.
  //
  // A brightness of 0..255 would otherwise need fifty clicks, so a brisk sweep
  // multiplies the step. A carousel of three views and a menu of four do not
  // want that: multiplying a list just makes it skip entries, and the thing you
  // were reaching for goes past.
  const int step = (rate >= kFastTurnRate) ? kFastTurnStep : 1;
  const int d = detents * step;

  switch (screen()) {
    case Screen::Brightness: {
      int v = ui_.brightness + d * 4;
      if (v < panel::kMinBrightness) v = panel::kMinBrightness;
      if (v > 255) v = 255;
      // Into the struct, then out to the live state — the same way round as
      // every other setting. Writing only UiState, as this did, is how the
      // timer length came to be forgotten at every power cut; brightness and
      // hue had the identical bug and it was never noticed because the
      // defaults happen to be close to what anyone picks.
      set_.brightness = static_cast<uint8_t>(v);
      apply_settings();
      note_change();
      break;
    }
    case Screen::ColorPick: {
      set_.hue = panel::wrap01(set_.hue + d * 0.02f);
      apply_settings();
      note_change();
      break;
    }
    case Screen::TimerSet: {
      int v = ui_.timer_set_min + d;
      if (v < 1) v = 1;
      if (v > 99) v = 99;
      // Into the stored struct, not only into the live state. Writing just
      // UiState meant the length you dialled was correct until the next power
      // cut and then quietly went back to twenty-five — a setting that
      // appeared to work and did not survive, which is the same defect the
      // settings tree exists to clear.
      set_.work_min = static_cast<uint16_t>(v);
      apply_settings();
      note_change();
      break;
    }
    case Screen::Menu: {
      // The list scrolls by changing the index and restarting the screen with a
      // disk transition. The outgoing frame still holds the previous entry,
      // which is the whole reason restart_with takes the state being left.
      //
      // By raw detents, not by one per frame: spinning three clicks should land
      // three entries on in one movement, rather than starting and abandoning
      // three transitions in thirty milliseconds, which is what made a fast
      // turn feel like the panel was fighting itself.
      // Both levels of the list scroll identically; only the length differs.
      const int n = ui_.list_count;
      if (n <= 0) break;
      const int next = wrap_index(static_cast<int>(ui_.menu_index) + detents, n);
      const TransitionKind k = detents > 0 ? TransitionKind::DiskUp : TransitionKind::DiskDown;
      mgr_.restart_with(ui_, k, panel::transition_seconds(k));
      ui_.menu_index = static_cast<uint8_t>(next);
      break;
    }

    case Screen::Group: {
      const int n = ui_.list_count > 0 ? ui_.list_count : 1;
      const int next = wrap_index(item_index_ + detents, n);
      const TransitionKind k = detents > 0 ? TransitionKind::DiskUp : TransitionKind::DiskDown;
      mgr_.restart_with(ui_, k, panel::transition_seconds(k));
      item_index_ = next;
      ui_.menu_index = static_cast<uint8_t>(next);
      refresh_group();
      break;
    }

    case Screen::Setting: {
      const SettingDesc* d = current_setting();
      if (!d) break;
      // Lists wrap and ranges clamp, which is the difference between choosing
      // and adjusting: a list has no ends worth stopping at, and a number run
      // off its end should stay there rather than reappear at the other.
      int v = setting_get(set_, d->id);
      if (d->kind == SettingKind::Number) {
        // The acceleration that applies to values applies here and nowhere
        // else: REST runs 0..120 in fives, which is a long way at one click a
        // step, while a list of four would only skip past what you wanted.
        v += detents * d->step * ((rate >= kFastTurnRate) ? kFastTurnStep : 1);
        if (v < d->lo) v = d->lo;
        if (v > d->hi) v = d->hi;
      } else {
        const int n = d->option_count ? d->option_count : (d->hi - d->lo + 1);
        v = d->lo + wrap_index(v - d->lo + detents, n);
      }
      setting_set(set_, d->id, v);
      apply_settings();
      note_change();
      refresh_setting();
      break;
    }

    case Screen::StatusPick: {
      const int n = static_cast<int>(Status::Count);
      const int next = wrap_index(static_cast<int>(ui_.pick) + detents, n);
      const TransitionKind k = detents > 0 ? TransitionKind::DiskUp : TransitionKind::DiskDown;
      mgr_.restart_with(ui_, k, panel::transition_seconds(k));
      ui_.pick = static_cast<Status>(next);
      break;
    }

    default:
      // At rest the knob moves through the home views, again by raw detents.
      if (detents != 0) goto_view(view_ + detents, detents);
      break;
  }
}

void App::handle(const Event& e, double now_s) {
  now_s_ = now_s;
  last_ = e.type;
  if (e.type == EventType::None) return;

  // Any input at all wakes the panel and restarts the idle clock. The gesture
  // that woke it is swallowed, so you never change your status by reaching for
  // a sleeping device.
  idle_s_ = 0.0f;
  // A celebration you cannot get out of is an obstacle. Any deliberate input
  // ends it, and is then acted on normally — unlike waking from sleep, where
  // the gesture is swallowed, because here you can see what you are doing.
  if (fl_.kind() == panel::FlourishKind::Done && fl_.active(now_s)) fl_.cancel();
  if (booting_) {
    // Touching it during the splash ends the splash and nothing else: you
    // should not be able to change your status by accident while it wakes up.
    booting_ = false;
    boot_s_ = panel::kBootSeconds;
    nav_[0] = NavFrame{kHomeViews[view_], TransitionKind::None};
    mgr_.go_to(kHomeViews[view_], ui_, TransitionKind::Fade,
               panel::transition_seconds(TransitionKind::Fade));
    return;
  }
  if (asleep_) {
    const bool wakeable = e.type != EventType::LaidFlat;
    if (wakeable) {
      wake();
      return;
    }
  }

  switch (e.type) {
    // ---------------------------------------------------------- touch
    case EventType::Tap:
      switch (static_cast<Zone>(e.which)) {
        case Zone::Left:
          set_status(ui_.status == Status::Busy ? Status::Free : Status::Busy);
          break;
        case Zone::Middle:
          if (ui_.timer_left_s > 0) ui_.timer_running = !ui_.timer_running;
          break;
        case Zone::Right:
          goto_view(view_ + 1, +1);
          break;
        default:
          break;
      }
      break;

    case EventType::DoubleTap:
      // Supersedes the tap that already fired, which is why every one of these
      // is an assignment rather than a toggle.
      if (static_cast<Zone>(e.which) == Zone::Left) set_status(Status::Call);
      break;

    case EventType::HoldBegin:
      switch (static_cast<Zone>(e.which)) {
        case Zone::Left:
          set_status(Status::Call);
          break;
        case Zone::Middle:
          reset_timer();
          break;
        case Zone::Right:
          set_status(ui_.status == Status::Dnd ? Status::Free : Status::Dnd);
          break;
        default:
          break;
      }
      break;

    case EventType::Chord:
      if (e.which == (zone_bit(Zone::Left) | zone_bit(Zone::Right))) sleep();
      break;

    case EventType::Swipe:
      goto_view(view_ + (e.delta > 0 ? 1 : -1), e.delta > 0 ? 1 : -1);
      break;

    // -------------------------------------------------------- encoder
    case EventType::Turn:
      adjust(e.delta, e.velocity);
      break;

    case EventType::Press:
      if (screen() == Screen::Menu || screen() == Screen::Group) {
        enter_menu_entry();
      } else if (screen() == Screen::StatusPick) {
        // Pressing commits what you were previewing, and the claim animation
        // plays on the way back out — onto the status view, whichever view you
        // were on before.
        //
        // go_home() returns to kHomeViews[view_], so claiming a status while
        // the knob had last been left on the clock put you back on the clock:
        // the one screen guaranteed not to show the thing you just chose. The
        // room sees a clock, and so do you.
        const Status chosen = ui_.pick;
        for (int i = 0; i < kHomeViewCount; ++i) {
          if (kHomeViews[i] == Screen::Status) {
            view_ = i;
            break;
          }
        }
        go_home();
        set_status(chosen);
      } else if (depth_ > 1) {
        // Inside an adjuster, pressing accepts and goes back one level — to the
        // menu you came from, not onward to some unrelated setting.
        pop();
      } else if (screen() == Screen::Draw) {
        drawing_ = false;
        go_home();
      } else if (is_net_screen(screen())) {
        // These are notices, so a press dismisses one. Without this the
        // address screen could only be left by turning the knob or waiting it
        // out, which is not something anybody would guess.
        net_info_s_ = 0.0f;
        go_home();
      } else if (screen() == Screen::Status) {
        set_status(ui_.status == Status::Busy ? Status::Free : Status::Busy);
      } else if (screen() == Screen::Timer) {
        if (ui_.timer_left_s > 0) ui_.timer_running = !ui_.timer_running;
      }
      break;

    case EventType::DoublePress:
      go_home();
      break;

    case EventType::PressHoldBegin:
      // Press goes in, hold goes out. One rule at every level, so there is
      // always a way back that does not depend on remembering how deep you are.
      if (depth_ == 1) {
        ui_.menu_index = 0;
        refresh_menu();
        push(Screen::Menu, TransitionKind::WipeUp);
      } else {
        pop();
      }
      break;

    // --------------------------------------------------------- motion
    case EventType::CaseTap:
      set_status(ui_.status == Status::Busy ? Status::Free : Status::Busy);
      break;

    case EventType::Shake:
      if (depth_ > 1) {
        go_home();
      }
      break;

    case EventType::LaidFlat:
      if (set_.flat_sleeps) sleep();
      break;

    case EventType::PickedUp:
      wake();
      break;

    default:
      break;
  }
}

}  // namespace ui
