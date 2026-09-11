#include "ui/app.h"

#include "panel/anim.h"

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
  if (brightness < 4) brightness = 4;  // never so dim the panel looks dead
  if (hue < 0.0f || hue > 1.0f) hue = 0.08f;
  if (sleep_after_min > 720) sleep_after_min = 720;
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

  ui_.brightness = set_.brightness;
  ui_.hue = set_.hue;
  ui_.accent = panel::accent_from_hue(set_.hue);
  ui_.timer_set_min = set_.work_min;
  ui_.timer_total_s = set_.work_min * 60;
  ui_.timer_left_s = ui_.timer_total_s;
  ui_.timer_running = false;

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
    }
    ui_.wifi_connected = ports_->wifi_connected();
  }

  // Leave the boot screen once it has been seen. A connected radio ends it
  // early, because at that point it has nothing left to tell you.
  if (booting_) {
    boot_s_ += dt_s;
    if (boot_s_ >= kBootSeconds || (ui_.wifi_connected && boot_s_ >= 0.4f)) {
      booting_ = false;
      nav_[0] = NavFrame{kHomeViews[view_], TransitionKind::None};
      mgr_.go_to(kHomeViews[view_], ui_, TransitionKind::Fade,
                 panel::transition_seconds(TransitionKind::Fade));
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
    if (ui_.timer_left_s <= 0) ui_.timer_running = false;
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
  mgr_.render(fb, ui_, a);
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
  mgr_.go_to(to, ui_, back, panel::transition_seconds(back));
}

void App::go_home() {
  while (depth_ > 1) {
    const TransitionKind back = inverse_of(nav_[depth_ - 1].enter_kind);
    --depth_;
    if (depth_ == 1) {
      mgr_.go_to(kHomeViews[view_], ui_, back, panel::transition_seconds(back));
      nav_[0] = NavFrame{kHomeViews[view_], TransitionKind::None};
    }
  }
}

void App::goto_view(int index) {
  const int next = wrap_index(index, kHomeViewCount);
  const bool forward = wrap_index(next - view_, kHomeViewCount) == 1;
  view_ = next;
  const TransitionKind k = forward ? TransitionKind::DiskUp : TransitionKind::DiskDown;
  depth_ = 1;
  nav_[0] = NavFrame{kHomeViews[view_], TransitionKind::None};
  if (kHomeViews[view_] == mgr_.current()) return;
  mgr_.go_to(kHomeViews[view_], ui_, k, panel::transition_seconds(k));
}

void App::set_status(Status s) {
  if (ui_.status == s) return;
  // A status change is the event this device exists for, so it takes the whole
  // panel wherever you happen to be standing.
  mgr_.restart_with(ui_, TransitionKind::Ignite,
                    panel::transition_seconds(TransitionKind::Ignite));
  ui_.status = s;
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
  // A brisk sweep steps faster, so a range of 0..255 does not need fifty
  // clicks — but a single deliberate detent is never multiplied.
  const int step = (rate >= kFastTurnRate) ? kFastTurnStep : 1;
  const int d = detents * step;

  switch (screen()) {
    case Screen::Brightness: {
      int v = ui_.brightness + d * 4;
      if (v < 4) v = 4;
      if (v > 255) v = 255;
      ui_.brightness = static_cast<uint8_t>(v);
      note_change();
      break;
    }
    case Screen::ColorPick: {
      ui_.hue = panel::wrap01(ui_.hue + d * 0.02f);
      ui_.accent = panel::accent_from_hue(ui_.hue);
      note_change();
      break;
    }
    case Screen::TimerSet: {
      int v = ui_.timer_set_min + d;
      if (v < 1) v = 1;
      if (v > 99) v = 99;
      ui_.timer_set_min = v;
      // Retarget a timer that has not been started, so the number you just
      // dialled is the number that runs.
      if (!ui_.timer_running) {
        ui_.timer_total_s = v * 60;
        ui_.timer_left_s = ui_.timer_total_s;
      }
      note_change();
      break;
    }
    default:
      // At rest the knob moves through the home views.
      if (d > 0) {
        goto_view(view_ + 1);
      } else if (d < 0) {
        goto_view(view_ - 1);
      }
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
  if (booting_) {
    // Touching it during the splash ends the splash and nothing else: you
    // should not be able to change your status by accident while it wakes up.
    booting_ = false;
    boot_s_ = kBootSeconds;
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
          goto_view(view_ + 1);
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
          ui_.timer_running = false;
          ui_.timer_total_s = ui_.timer_set_min * 60;
          ui_.timer_left_s = ui_.timer_total_s;
          timer_accum_s_ = 0.0f;
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
      goto_view(view_ + (e.delta > 0 ? 1 : -1));
      break;

    // -------------------------------------------------------- encoder
    case EventType::Turn:
      adjust(e.delta, e.velocity);
      break;

    case EventType::PressTurn: {
      // Brightness from anywhere, without leaving the screen you are on.
      int v = ui_.brightness + e.delta * 6;
      if (v < 4) v = 4;
      if (v > 255) v = 255;
      ui_.brightness = static_cast<uint8_t>(v);
      note_change();
      break;
    }

    case EventType::Press:
      if (depth_ > 1) {
        // Step through the adjust screens, then back out the far end.
        adjust_ = adjust_ + 1;
        if (adjust_ >= kAdjustViewCount) {
          adjust_ = 0;
          go_home();
        } else {
          nav_[depth_ - 1].screen = kAdjustViews[adjust_];
          mgr_.go_to(kAdjustViews[adjust_], ui_, TransitionKind::DiskUp,
                     panel::transition_seconds(TransitionKind::DiskUp));
        }
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
      if (depth_ == 1) {
        adjust_ = 0;
        push(kAdjustViews[0], TransitionKind::WipeUp);
      } else {
        go_home();
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
