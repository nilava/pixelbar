// Host tests for the input layer.
//
// Every one of these drives the recogniser with a synthetic trace at a fixed
// tick and asserts on exactly what came out. That is the whole reason the
// recogniser is free of ESP-IDF: a gesture is nothing but edge cases, and none
// of them are worth discovering with a finger on a soldered board.
#include <cstring>
#include <vector>

#include "harness.h"
#include "panel/config.h"
#include "ui/app.h"
#include "ui/gesture.h"

using namespace ui;

namespace {

constexpr float kTick = 0.01f;  // the 100 fps frame, which is what the device uses

// Drives the recogniser and collects everything it emits.
class Rig {
 public:
  explicit Rig(const GestureConfig& cfg = GestureConfig{}) : rec_(cfg) {}

  void touch(int zone, bool down) { in_.touch[zone] = down; }
  void sw(bool down) { in_.encoder_sw = down; }
  void turn(int detents) { in_.encoder_detents += detents; }
  void accel(float x, float y, float z) {
    in_.ax = x;
    in_.ay = y;
    in_.az = z;
    in_.motion_valid = true;
  }

  // Advances one frame and appends whatever came out.
  void step(float dt = kTick) {
    Event ev[kMaxEventsPerFrame];
    const int n = rec_.update(in_, dt, ev, kMaxEventsPerFrame);
    for (int i = 0; i < n; ++i) got_.push_back(ev[i]);
  }
  void hold_for(float seconds) {
    const int frames = static_cast<int>(seconds / kTick + 0.5f);
    for (int i = 0; i < frames; ++i) step();
  }
  // A finger dragged across the pads: each contact overlaps the next briefly
  // and is released as the finger moves on. Three pads held at once is a
  // chord, not a swipe, which is exactly the distinction being tested.
  void drag(int a, int b, int c) {
    touch(a, true);
    hold_for(0.05f);
    touch(b, true);
    hold_for(0.03f);
    touch(a, false);
    hold_for(0.03f);
    touch(c, true);
    hold_for(0.03f);
    touch(b, false);
    hold_for(0.05f);
    touch(c, false);
    hold_for(0.05f);
  }

  // A press and release of `seconds`, then settled.
  void tap_zone(int zone, float seconds = 0.05f) {
    touch(zone, true);
    hold_for(seconds);
    touch(zone, false);
    hold_for(0.02f);
  }

  void clear() { got_.clear(); }
  const std::vector<Event>& got() const { return got_; }

  int count(EventType t) const {
    int n = 0;
    for (const Event& e : got_)
      if (e.type == t) ++n;
    return n;
  }
  bool saw(EventType t, int which = -1) const {
    for (const Event& e : got_)
      if (e.type == t && (which < 0 || e.which == which)) return true;
    return false;
  }
  // The index of the first event of this type, or -1.
  int index_of(EventType t) const {
    for (size_t i = 0; i < got_.size(); ++i)
      if (got_[i].type == t) return static_cast<int>(i);
    return -1;
  }

  Recogniser& rec() { return rec_; }

 private:
  Recogniser rec_;
  RawInput in_;
  std::vector<Event> got_;
};

void test_quadrature() {
  CASE("four quarter steps make one detent, in both directions");
  {
    Quadrature q;
    // The Gray sequence for one direction: 00 -> 10 -> 11 -> 01 -> 00.
    const bool a[4] = {false, true, true, false};
    const bool b[4] = {false, false, true, true};
    q.update(a[0], b[0]);  // prime
    int detents = 0;
    for (int rev = 0; rev < 3; ++rev)
      for (int i = 1; i <= 4; ++i) detents += q.update(a[i % 4], b[i % 4]);
    CHECK_EQ(detents, 3);
    CHECK_EQ(q.detents(), 3);
    CHECK_EQ(q.illegal(), 0);

    Quadrature r;
    r.update(a[0], b[0]);
    int back = 0;
    for (int rev = 0; rev < 3; ++rev)
      for (int i = 3; i >= 0; --i) back += r.update(a[i], b[i]);
    CHECK_EQ(back, -3);
    CHECK_EQ(r.illegal(), 0);
  }

  CASE("a partial turn produces no detent, and does not lose its place");
  {
    Quadrature q;
    q.update(false, false);
    CHECK_EQ(q.update(true, false), 0);   // one quarter
    CHECK_EQ(q.update(true, true), 0);    // two
    CHECK_EQ(q.update(true, false), 0);   // back to one
    CHECK_EQ(q.update(false, false), 0);  // home again
    CHECK_EQ(q.detents(), 0);
    CHECK_EQ(q.illegal(), 0);
  }

  CASE("both lines moving at once is counted, not guessed at");
  {
    // This is the signature of a sampler that is being starved. Guessing a
    // direction here would make a fast spin drift instead of stall, which is a
    // far harder fault to recognise on hardware.
    Quadrature q;
    q.update(false, false);
    q.update(true, true);  // illegal: 00 -> 11
    CHECK_EQ(q.illegal(), 1);
    CHECK_EQ(q.detents(), 0);
  }
}

void test_taps() {
  CASE("a tap fires on release");
  {
    Rig r;
    r.tap_zone(0);
    CHECK_EQ(r.count(EventType::Tap), 1);
    CHECK(r.saw(EventType::Tap, 0));
    CHECK_EQ(r.count(EventType::HoldBegin), 0);
  }

  CASE("a tap is never held back waiting to see if it doubles");
  {
    // The rule the whole design turns on: the most-used gesture on the device
    // must not pay 280 ms for a gesture that usually does not happen.
    Rig r;
    r.touch(0, true);
    r.hold_for(0.05f);
    r.touch(0, false);
    r.step();
    r.step();  // one frame for the debounce to settle, and that is all
    CHECK_EQ(r.count(EventType::Tap), 1);
  }

  CASE("a second tap inside the window supersedes rather than repeats");
  {
    Rig r;
    r.tap_zone(0);
    r.tap_zone(0);
    CHECK_EQ(r.count(EventType::Tap), 1);        // only the first
    CHECK_EQ(r.count(EventType::DoubleTap), 1);  // and then the upgrade
    CHECK(r.index_of(EventType::Tap) < r.index_of(EventType::DoubleTap));
  }

  CASE("two taps outside the window are two taps");
  {
    Rig r;
    r.tap_zone(0);
    r.hold_for(0.40f);  // past double_tap_s
    r.tap_zone(0);
    CHECK_EQ(r.count(EventType::Tap), 2);
    CHECK_EQ(r.count(EventType::DoubleTap), 0);
  }

  CASE("taps on different zones never double each other");
  {
    Rig r;
    r.tap_zone(0);
    r.tap_zone(1);
    CHECK_EQ(r.count(EventType::Tap), 2);
    CHECK_EQ(r.count(EventType::DoubleTap), 0);
  }

  CASE("a hold begins once, while still held, and ends on release");
  {
    Rig r;
    r.touch(2, true);
    r.hold_for(0.30f);
    CHECK_EQ(r.count(EventType::HoldBegin), 0);  // not yet
    r.hold_for(0.30f);                           // past hold_s
    CHECK_EQ(r.count(EventType::HoldBegin), 1);
    r.hold_for(1.0f);
    CHECK_EQ(r.count(EventType::HoldBegin), 1);  // still once
    CHECK_EQ(r.count(EventType::HoldEnd), 0);
    r.touch(2, false);
    r.hold_for(0.03f);
    CHECK_EQ(r.count(EventType::HoldEnd), 1);
    CHECK_EQ(r.count(EventType::Tap), 0);  // a hold is not a tap
  }

  CASE("a hold cannot be doubled by a following tap");
  {
    Rig r;
    r.touch(0, true);
    r.hold_for(0.60f);
    r.touch(0, false);
    r.hold_for(0.05f);
    r.tap_zone(0);
    CHECK_EQ(r.count(EventType::DoubleTap), 0);
    CHECK_EQ(r.count(EventType::Tap), 1);
  }

  CASE("a stray single sample is rejected");
  {
    Rig r;
    r.touch(1, true);
    r.step(0.001f);  // shorter than touch_stable_s
    r.touch(1, false);
    r.hold_for(0.05f);
    CHECK_EQ(r.count(EventType::Tap), 0);
  }
}

void test_chord_and_swipe() {
  CASE("two zones together are one chord, not two taps");
  {
    Rig r;
    r.touch(0, true);
    r.step();
    r.step();
    r.touch(2, true);
    r.hold_for(0.20f);  // held past chord_s, which is what makes it a chord
    CHECK_EQ(r.count(EventType::Chord), 1);
    r.touch(0, false);
    r.touch(2, false);
    r.hold_for(0.05f);
    CHECK_EQ(r.count(EventType::Tap), 0);  // neither zone taps
    CHECK_EQ(r.count(EventType::Chord), 1);
  }

  CASE("a chord names both zones");
  {
    Rig r;
    r.touch(0, true);
    r.touch(2, true);
    r.hold_for(0.20f);
    const int i = r.index_of(EventType::Chord);
    CHECK(i >= 0);
    if (i >= 0) {
      CHECK_EQ(r.got()[i].which,
               zone_bit(Zone::Left) | zone_bit(Zone::Right));
    }
  }

  CASE("two zones one after the other are two separate taps");
  {
    Rig r;
    r.tap_zone(0);
    r.hold_for(0.40f);
    r.tap_zone(2);
    CHECK_EQ(r.count(EventType::Chord), 0);
    CHECK_EQ(r.count(EventType::Tap), 2);
  }

  CASE("two zones held together but let go too soon do nothing at all");
  {
    // Not long enough to be a chord, and not a shape either pad should report
    // on its own. An ambiguous gesture is better ignored than guessed at: the
    // left pad toggles your status, and firing that during a fumbled chord is
    // worse than the gesture simply not registering.
    Rig r;
    r.touch(0, true);
    r.hold_for(0.02f);
    r.touch(2, true);
    r.hold_for(0.05f);  // short of chord_s
    r.touch(0, false);
    r.touch(2, false);
    r.hold_for(0.05f);
    CHECK_EQ(r.count(EventType::Chord), 0);
    CHECK_EQ(r.count(EventType::Tap), 0);
  }

  CASE("three zones in order are a swipe, in the direction they ran");
  {
    Rig r;
    r.drag(0, 1, 2);
    CHECK_EQ(r.count(EventType::Swipe), 1);
    const int i = r.index_of(EventType::Swipe);
    if (i >= 0) CHECK_EQ(r.got()[i].delta, 1);

    Rig l;
    l.drag(2, 1, 0);
    CHECK_EQ(l.count(EventType::Swipe), 1);
    const int j = l.index_of(EventType::Swipe);
    if (j >= 0) CHECK_EQ(l.got()[j].delta, -1);
  }

  CASE("a swipe emits no taps for the zones it crossed");
  {
    Rig r;
    r.drag(0, 1, 2);
    CHECK_EQ(r.count(EventType::Tap), 0);
    CHECK_EQ(r.count(EventType::Swipe), 1);
  }

  CASE("zones touched out of order are not a swipe");
  {
    Rig r;
    r.drag(0, 2, 1);
    CHECK_EQ(r.count(EventType::Swipe), 0);
  }

  CASE("a slow sweep across the zones is not a swipe");
  {
    Rig r;
    r.touch(0, true);
    r.hold_for(0.05f);
    r.touch(0, false);
    r.hold_for(0.40f);  // past swipe_step_s
    r.touch(1, true);
    r.hold_for(0.05f);
    r.touch(1, false);
    r.hold_for(0.40f);
    r.touch(2, true);
    r.hold_for(0.05f);
    r.touch(2, false);
    r.hold_for(0.05f);
    CHECK_EQ(r.count(EventType::Swipe), 0);
  }
}

void test_chord_is_not_a_swipe() {
  CASE("three pads pressed and held is a chord, not a swipe");
  {
    // The case that made the naive timing rule wrong: pressing the pads in
    // order and keeping them all down looks like a swipe until you notice
    // nothing was released.
    Rig r;
    r.touch(0, true);
    r.hold_for(0.05f);
    r.touch(1, true);
    r.hold_for(0.05f);
    r.touch(2, true);
    r.hold_for(0.25f);
    CHECK_EQ(r.count(EventType::Swipe), 0);
    CHECK_EQ(r.count(EventType::Chord), 1);
  }

  CASE("a drag that brushes two pads together is still a swipe");
  {
    Rig r;
    r.drag(0, 1, 2);
    CHECK_EQ(r.count(EventType::Chord), 0);
    CHECK_EQ(r.count(EventType::Swipe), 1);
  }
}

void test_encoder() {
  CASE("a detent turns into one event carrying its direction");
  {
    Rig r;
    r.turn(1);
    r.step();
    CHECK_EQ(r.count(EventType::Turn), 1);
    const int i = r.index_of(EventType::Turn);
    if (i >= 0) CHECK_EQ(r.got()[i].delta, 1);
    r.clear();
    r.turn(-3);
    r.step();
    const int j = r.index_of(EventType::Turn);
    if (j >= 0) CHECK_EQ(r.got()[j].delta, -3);
  }

  CASE("a frame that sees no movement emits nothing");
  {
    Rig r;
    r.hold_for(0.5f);
    CHECK_EQ(r.count(EventType::Turn), 0);
  }

  CASE("detents accumulated between frames are never lost");
  {
    // The count is accumulated by the interrupt, not sampled, so a late frame
    // reports everything that happened while it was away.
    Rig r;
    for (int i = 0; i < 12; ++i) r.turn(1);
    r.step(0.2f);  // one very long frame
    int total = 0;
    for (const Event& e : r.got())
      if (e.type == EventType::Turn) total += e.delta;
    CHECK_EQ(total, 12);
  }

  CASE("a press fires on release, and doubles like a tap");
  {
    // The release is acted on once it has settled, so it lands a frame or two
    // after the contact opens rather than on the same one. Fifteen
    // milliseconds is not something a finger can feel, and it is what stops the
    // rotary contacts' ground bounce from being read as a click.
    Rig r;
    r.sw(true);
    r.hold_for(0.05f);
    r.sw(false);
    r.hold_for(0.03f);
    CHECK_EQ(r.count(EventType::Press), 1);
    r.sw(true);
    r.hold_for(0.05f);
    r.sw(false);
    r.hold_for(0.03f);
    CHECK_EQ(r.count(EventType::DoublePress), 1);
    CHECK_EQ(r.count(EventType::Press), 1);
  }

  CASE("holding the knob is its own gesture");
  {
    Rig r;
    r.sw(true);
    r.hold_for(0.60f);
    CHECK_EQ(r.count(EventType::PressHoldBegin), 1);
    CHECK_EQ(r.count(EventType::Press), 0);
    r.sw(false);
    r.hold_for(0.03f);
    CHECK_EQ(r.count(EventType::PressHoldEnd), 1);
    CHECK_EQ(r.count(EventType::Press), 0);
  }

  CASE("turning while the knob is held is just a turn, and does not click");
  {
    // There is no press-and-turn gesture. It duplicated a path the menu already
    // provides, and on this hardware the push switch shares its ground with the
    // rotary contacts, so an ordinary turn kept being read as the modifier.
    Rig r;
    r.sw(true);
    r.hold_for(0.15f);
    r.turn(2);
    r.step();
    CHECK_EQ(r.count(EventType::Turn), 1);
    r.sw(false);
    r.hold_for(0.03f);
    // Releasing after a turn must not also click, or every adjustment would
    // end by activating whatever the press does.
    CHECK_EQ(r.count(EventType::Press), 0);
    CHECK_EQ(r.count(EventType::PressHoldBegin), 0);
  }

  CASE("a fast sweep reports a higher rate than a slow one");
  {
    Rig fast;
    for (int i = 0; i < 40; ++i) {
      fast.turn(1);
      fast.step();
    }
    Rig slow;
    for (int i = 0; i < 40; ++i) {
      slow.turn(1);
      slow.hold_for(0.10f);
    }
    float fast_peak = 0.0f, slow_peak = 0.0f;
    for (const Event& e : fast.got())
      if (e.type == EventType::Turn && e.velocity > fast_peak) fast_peak = e.velocity;
    for (const Event& e : slow.got())
      if (e.type == EventType::Turn && e.velocity > slow_peak) slow_peak = e.velocity;
    CHECK(fast_peak > slow_peak * 2.0f);
  }
}


void test_switch_noise() {
  CASE("a glitch on the switch while turning is not a press-and-turn");
  {
    // The bug this pins, seen on the first board: on a KY-040 the push switch
    // shares its ground net with the rotary contacts, so turning the knob
    // bounces that ground and the switch line dips with it. Untreated, every
    // ordinary turn also adjusted the brightness, and the panel fought between
    // changing screen and changing value.
    Rig r;
    for (int i = 0; i < 20; ++i) {
      r.sw(true);       // a one-frame dip, exactly what ground bounce looks like
      r.turn(1);
      r.step();
      r.sw(false);
      r.step();
      r.step();
    }
    CHECK_EQ(r.count(EventType::PressTurn), 0);
    CHECK_EQ(r.count(EventType::Turn), 20);  // every turn still counted, once
  }

  CASE("and it is not a press either");
  {
    Rig r;
    for (int i = 0; i < 20; ++i) {
      r.sw(true);
      r.step();
      r.sw(false);
      r.step();
      r.step();
    }
    CHECK_EQ(r.count(EventType::Press), 0);
    CHECK_EQ(r.count(EventType::DoublePress), 0);
    CHECK_EQ(r.count(EventType::PressHoldBegin), 0);
  }

  CASE("a deliberate press still registers, and is not slowed much by the filter");
  {
    Rig r;
    r.sw(true);
    r.hold_for(0.06f);  // a short but real press
    r.sw(false);
    r.hold_for(0.05f);
    CHECK_EQ(r.count(EventType::Press), 1);
  }

  CASE("a glitch never produces a turn that was not there");
  {
    // The switch dipping is not supposed to add, remove or alter turns. This
    // is the other half of the same bug: whatever the switch line does, the
    // count of turns has to match the count of detents.
    Rig r;
    for (int i = 0; i < 12; ++i) {
      r.sw(i % 2 == 0);
      r.turn(1);
      r.step();
    }
    int total = 0;
    for (const Event& e : r.got())
      if (e.type == EventType::Turn) total += e.delta;
    CHECK_EQ(total, 12);
  }
}

void test_motion() {
  CASE("a knock is a tap, a wallop is a shake, and one is not both");
  {
    GestureConfig cfg;
    Rig r(cfg);
    r.accel(1.0f, 0.0f, 0.0f);
    r.hold_for(0.2f);
    r.clear();
    r.accel(1.0f + cfg.tap_g + 0.1f, 0.0f, 0.0f);  // a jolt over the tap threshold
    r.step();
    CHECK_EQ(r.count(EventType::CaseTap), 1);
    CHECK_EQ(r.count(EventType::Shake), 0);

    Rig s(cfg);
    s.accel(1.0f, 0.0f, 0.0f);
    s.hold_for(0.2f);
    s.clear();
    s.accel(1.0f + cfg.shake_g + 0.2f, 0.0f, 0.0f);
    s.step();
    CHECK_EQ(s.count(EventType::Shake), 1);
    CHECK_EQ(s.count(EventType::CaseTap), 0);
  }

  CASE("one knock does not become a burst of them");
  {
    GestureConfig cfg;
    Rig r(cfg);
    r.accel(1.0f, 0.0f, 0.0f);
    r.hold_for(0.2f);
    r.clear();
    for (int i = 0; i < 6; ++i) {
      r.accel(1.0f + cfg.tap_g + 0.1f, 0.0f, 0.0f);
      r.step();
      r.accel(1.0f, 0.0f, 0.0f);
      r.step();
    }
    CHECK(r.count(EventType::CaseTap) <= 2);  // lock-out holds
  }

  CASE("laying it down and picking it up need a steady second");
  {
    Rig r;
    r.accel(0.0f, 1.0f, 0.0f);  // standing: gravity in the panel plane
    r.hold_for(2.0f);
    r.clear();

    r.accel(0.0f, 0.0f, 1.0f);  // laid flat: gravity on the board normal
    r.hold_for(0.5f);
    CHECK_EQ(r.count(EventType::LaidFlat), 0);  // not yet, on purpose
    r.hold_for(0.8f);
    CHECK_EQ(r.count(EventType::LaidFlat), 1);

    r.clear();
    r.accel(0.0f, 1.0f, 0.0f);
    r.hold_for(1.5f);
    CHECK_EQ(r.count(EventType::PickedUp), 1);
  }

  CASE("a wobble while it stands does not report anything");
  {
    Rig r;
    r.accel(0.0f, 1.0f, 0.0f);
    r.hold_for(2.0f);
    r.clear();
    for (int i = 0; i < 30; ++i) {
      r.accel(0.0f, i % 2 ? 0.95f : 1.05f, 0.0f);
      r.step();
    }
    CHECK_EQ(r.count(EventType::LaidFlat), 0);
    CHECK_EQ(r.count(EventType::CaseTap), 0);
    CHECK_EQ(r.count(EventType::Shake), 0);
  }

  CASE("with no sensor the motion layer is silent, not wrong");
  {
    Rig r;  // never calls accel(), so motion_valid stays false
    r.hold_for(3.0f);
    CHECK_EQ(r.count(EventType::LaidFlat), 0);
    CHECK_EQ(r.count(EventType::CaseTap), 0);
  }
}

void test_no_crosstalk() {
  CASE("a quiet frame emits nothing at all");
  {
    Rig r;
    r.hold_for(2.0f);
    CHECK_EQ(r.got().size(), 0u);
  }

  CASE("the emit buffer is never overrun");
  {
    // Everything at once, into a buffer of one.
    Recogniser rec;
    RawInput in;
    in.touch[0] = in.touch[1] = in.touch[2] = true;
    in.encoder_sw = true;
    in.encoder_detents = 5;
    Event one[1];
    const int n = rec.update(in, 0.5f, one, 1);
    CHECK(n <= 1);
  }
}


// ------------------------------------------------------------------ the app

// Ports backed by memory: settings live in a member, the clock is synthetic,
// and the raw input is whatever the test last set.
class TestPorts : public Ports {
 public:
  void read_raw(RawInput* out) override { *out = raw; }
  bool load_settings(Settings* out) override {
    if (!valid) return false;
    *out = stored;
    return true;
  }
  bool save_settings(const Settings& s) override {
    stored = s;
    valid = true;
    ++saves;
    return true;
  }
  bool wall_clock(int* h, int* m, int* s) override {
    *h = 14; *m = 25; *s = 0;
    return has_clock;
  }
  bool wifi_connected() override { return wifi; }

  RawInput raw;
  Settings stored;
  bool valid = false;
  bool has_clock = true;
  bool wifi = false;
  int saves = 0;
};

// Drives an App the way the firmware and the simulator do.
class AppRig {
 public:
  AppRig() {
    app.begin(ports, 0.0);
    // Past the boot sequence *and* the fade that hands over from it, so a test
    // that starts by turning the knob is not competing with it.
    run(panel::kBootSeconds + 0.8f);
  }
  void run(float seconds) {
    const int frames = static_cast<int>(seconds / kTick + 0.5f);
    for (int i = 0; i < frames; ++i) {
      app.update(kTick, t_);
      t_ += kTick;
    }
  }
  void tap(int zone) {
    ports.raw.touch[zone] = true;
    run(0.06f);
    ports.raw.touch[zone] = false;
    run(0.05f);
  }
  void hold(int zone, float seconds = 0.6f) {
    ports.raw.touch[zone] = true;
    run(seconds);
    ports.raw.touch[zone] = false;
    run(0.05f);
  }
  void chord(int a, int b, float seconds = 0.25f) {
    ports.raw.touch[a] = true;
    ports.raw.touch[b] = true;
    run(seconds);
    ports.raw.touch[a] = false;
    ports.raw.touch[b] = false;
    run(0.05f);
  }
  void turn(int detents) {
    ports.raw.encoder_detents += detents;
    run(0.05f);
  }
  void press(float seconds = 0.06f) {
    ports.raw.encoder_sw = true;
    run(seconds);
    ports.raw.encoder_sw = false;
    run(0.05f);
  }
  void settle() { run(1.2f); }  // long enough for any transition to finish

  // Renders on the rig's own clock. Passing an arbitrary Anim::t instead would
  // put the draw behind the model's timeline, and every element that animates
  // against an absolute stamp — digit rolls, status swaps — would be caught
  // mid-move by a clock that had gone backwards.
  void render(panel::Framebuffer& fb) {
    panel::Anim a;
    a.dt = kTick;
    a.t = t_;
    app.render(fb, a);
  }

  // Advances until the timer hits zero, or gives up. Running a fixed 62 s
  // instead would sail straight past a flourish that only lasts 1.6.
  void run_until_timer_ends(float limit_s = 120.0f) {
    const int frames = static_cast<int>(limit_s / kTick);
    for (int i = 0; i < frames; ++i) {
      app.update(kTick, t_);
      t_ += kTick;
      if (app.state().timer_left_s <= 0) return;
    }
  }

  double now() const { return t_; }

  // Hold the knob to open the menu, scroll to a labelled entry, press it.
  void open_menu() { press(0.7f); settle(); }
  void menu_to(const char* label) {
    for (int guard = 0; guard < panel::kMenuCount + 1; ++guard) {
      const int i = app.state().menu_index;
      const char* here = panel::kMenu[i].label;
      if (std::strcmp(here, label) == 0) return;
      turn(1);
      settle();
    }
  }
  void enter(const char* label) {
    open_menu();
    menu_to(label);
    press();
    settle();
  }

  TestPorts ports;
  App app;

 private:
  double t_ = 0.0;
};

void test_app_boot_and_views() {
  CASE("the panel leaves the boot screen on its own");
  {
    TestPorts p;
    App a;
    a.begin(p, 0.0);
    CHECK_EQ(static_cast<int>(a.screen()), static_cast<int>(panel::Screen::Booting));
    double t = 0.0;
    const int frames = static_cast<int>((panel::kBootSeconds + 0.8f) / kTick);
    for (int i = 0; i < frames; ++i) { a.update(kTick, t); t += kTick; }
    CHECK_EQ(static_cast<int>(a.screen()), static_cast<int>(panel::Screen::Status));
  }

  CASE("touching it during the splash only ends the splash");
  {
    // Reaching for a device that is still waking up must not change what it
    // is telling the room.
    AppRig r;  // already past boot
    TestPorts p;
    App a;
    a.begin(p, 0.0);
    double t = 0.0;
    for (int i = 0; i < 20; ++i) { a.update(kTick, t); t += kTick; }
    p.raw.touch[0] = true;
    for (int i = 0; i < 8; ++i) { a.update(kTick, t); t += kTick; }
    p.raw.touch[0] = false;
    for (int i = 0; i < 40; ++i) { a.update(kTick, t); t += kTick; }
    CHECK_EQ(static_cast<int>(a.state().status), static_cast<int>(panel::Status::Free));
  }

  CASE("the right pad and the knob both walk the home views, and wrap");
  {
    AppRig r;
    CHECK_EQ(static_cast<int>(r.app.screen()), static_cast<int>(panel::Screen::Status));
    r.tap(2); r.settle();
    CHECK_EQ(static_cast<int>(r.app.screen()), static_cast<int>(panel::Screen::Clock));
    r.tap(2); r.settle();
    CHECK_EQ(static_cast<int>(r.app.screen()), static_cast<int>(panel::Screen::Timer));
    r.tap(2); r.settle();
    CHECK_EQ(static_cast<int>(r.app.screen()), static_cast<int>(panel::Screen::Status));

    r.turn(1); r.settle();
    CHECK_EQ(static_cast<int>(r.app.screen()), static_cast<int>(panel::Screen::Clock));
    r.turn(-1); r.settle();
    CHECK_EQ(static_cast<int>(r.app.screen()), static_cast<int>(panel::Screen::Status));
    r.turn(-1); r.settle();  // wrapping backwards
    CHECK_EQ(static_cast<int>(r.app.screen()), static_cast<int>(panel::Screen::Timer));
  }

  CASE("a swipe walks the views too, and in the direction it ran");
  {
    AppRig r;
    // A drag: pads overlap and release as the finger moves on.
    r.ports.raw.touch[0] = true; r.run(0.05f);
    r.ports.raw.touch[1] = true; r.run(0.03f);
    r.ports.raw.touch[0] = false; r.run(0.03f);
    r.ports.raw.touch[2] = true; r.run(0.03f);
    r.ports.raw.touch[1] = false; r.run(0.05f);
    r.ports.raw.touch[2] = false; r.settle();
    CHECK_EQ(static_cast<int>(r.app.screen()), static_cast<int>(panel::Screen::Clock));
  }
}

void test_app_status() {
  CASE("the left pad toggles between free and busy");
  {
    AppRig r;
    CHECK_EQ(static_cast<int>(r.app.state().status), static_cast<int>(panel::Status::Free));
    r.tap(0); r.settle();
    CHECK_EQ(static_cast<int>(r.app.state().status), static_cast<int>(panel::Status::Busy));
    r.tap(0); r.settle();
    CHECK_EQ(static_cast<int>(r.app.state().status), static_cast<int>(panel::Status::Free));
  }

  CASE("a double tap lands on CALL, whatever the first tap did on the way");
  {
    // This is the supersede rule the recogniser is built around, seen from the
    // other end: the first tap has already changed the status, and the second
    // has to be an assignment that overwrites it rather than another toggle.
    AppRig r;
    r.tap(0);
    r.tap(0);
    r.settle();
    CHECK_EQ(static_cast<int>(r.app.state().status), static_cast<int>(panel::Status::Call));
  }

  CASE("holding the right pad toggles do-not-disturb");
  {
    AppRig r;
    r.hold(2); r.settle();
    CHECK_EQ(static_cast<int>(r.app.state().status), static_cast<int>(panel::Status::Dnd));
    r.hold(2); r.settle();
    CHECK_EQ(static_cast<int>(r.app.state().status), static_cast<int>(panel::Status::Free));
  }

  CASE("a status change never leaves the panel on the wrong screen");
  {
    AppRig r;
    r.tap(2); r.settle();  // go to the clock
    r.tap(0); r.settle();  // change status from there
    CHECK_EQ(static_cast<int>(r.app.state().status), static_cast<int>(panel::Status::Busy));
    CHECK_EQ(static_cast<int>(r.app.screen()), static_cast<int>(panel::Screen::Clock));
  }
}

void test_app_timer() {
  CASE("the middle pad starts and pauses, and the timer actually counts");
  {
    AppRig r;
    const int start = r.app.state().timer_left_s;
    CHECK(start > 0);
    r.run(2.0f);
    CHECK_EQ(r.app.state().timer_left_s, start);  // nothing until it is started

    r.tap(1);
    CHECK(r.app.state().timer_running);
    r.run(3.0f);
    CHECK(r.app.state().timer_left_s < start);
    const int mid = r.app.state().timer_left_s;

    r.tap(1);
    CHECK(!r.app.state().timer_running);
    r.run(3.0f);
    CHECK_EQ(r.app.state().timer_left_s, mid);  // paused means paused
  }

  CASE("holding the middle pad resets it to the set length");
  {
    AppRig r;
    r.tap(1);
    r.run(5.0f);
    CHECK(r.app.state().timer_left_s < r.app.state().timer_total_s);
    r.hold(1);
    CHECK(!r.app.state().timer_running);
    CHECK_EQ(r.app.state().timer_left_s, r.app.state().timer_set_min * 60);
  }
}

void test_app_adjust() {
  CASE("press goes in and hold goes out, at every level");
  {
    // One rule the whole way down, so there is always a way back that does not
    // depend on remembering how deep you are.
    AppRig r;
    CHECK_EQ(r.app.depth(), 1);
    r.open_menu();
    CHECK_EQ(r.app.depth(), 2);
    CHECK_EQ(static_cast<int>(r.app.screen()), static_cast<int>(panel::Screen::Menu));

    r.menu_to("DIM");
    r.press(); r.settle();
    CHECK_EQ(r.app.depth(), 3);
    CHECK_EQ(static_cast<int>(r.app.screen()), static_cast<int>(panel::Screen::Brightness));

    // Pressing accepts and returns to the menu you came from, rather than
    // stepping on to some unrelated setting.
    r.press(); r.settle();
    CHECK_EQ(r.app.depth(), 2);
    CHECK_EQ(static_cast<int>(r.app.screen()), static_cast<int>(panel::Screen::Menu));

    // And holding backs out of the menu the same way it opened it.
    r.press(0.7f); r.settle();
    CHECK_EQ(r.app.depth(), 1);
    CHECK_EQ(static_cast<int>(r.app.screen()), static_cast<int>(panel::Screen::Status));
  }

  CASE("every menu entry goes to exactly one place");
  {
    const struct { const char* label; panel::Screen screen; } kWant[] = {
        {"STAT", panel::Screen::StatusPick},
        {"TIME", panel::Screen::TimerSet},
        {"DIM", panel::Screen::Brightness},
        {"HUE", panel::Screen::ColorPick},
    };
    for (const auto& w : kWant) {
      AppRig r;
      r.enter(w.label);
      CHECK_EQ(static_cast<int>(r.app.screen()), static_cast<int>(w.screen));
      r.press();
      r.settle();
      if (w.screen == panel::Screen::StatusPick) {
        // The one deliberate exception. Pressing here does not accept a setting
        // and step back, it *claims a status* — and the result of that is the
        // room-facing screen, so showing you the menu again would be hiding the
        // thing you just did.
        CHECK_EQ(static_cast<int>(r.app.screen()), static_cast<int>(panel::Screen::Status));
        CHECK_EQ(r.app.depth(), 1);
      } else {
        CHECK_EQ(static_cast<int>(r.app.screen()), static_cast<int>(panel::Screen::Menu));
      }
    }
  }

  CASE("turning on the brightness screen changes brightness, and clamps");
  {
    AppRig r;
    r.enter("DIM");
    const int before = r.app.state().brightness;
    r.turn(5); r.settle();
    CHECK(r.app.state().brightness > before);
    for (int i = 0; i < 40; ++i) r.turn(5);
    CHECK_EQ(r.app.state().brightness, 255);
    for (int i = 0; i < 80; ++i) r.turn(-5);
    // The floor is measured, not chosen for looks: below it a growing share of
    // the panel sits at a PWM value of one to three, where these LEDs are least
    // well behaved.
    CHECK_EQ(r.app.state().brightness, panel::kMinBrightness);
  }

  CASE("the colour picker commits its hue to the accent");
  {
    AppRig r;
    r.enter("HUE");
    const panel::RGB before = r.app.state().accent;
    r.turn(6); r.settle();
    const panel::RGB after = r.app.state().accent;
    CHECK(!(before == after));
    CHECK(after == panel::accent_from_hue(r.app.state().hue));
  }

  CASE("a shake backs out of an adjuster but does nothing at home");
  {
    AppRig r;
    r.enter("DIM");
    CHECK(r.app.depth() > 1);
    r.app.handle(Event(EventType::Shake), 0.0);
    r.settle();
    CHECK_EQ(r.app.depth(), 1);
    const int screen_before = static_cast<int>(r.app.screen());
    r.app.handle(Event(EventType::Shake), 0.0);
    r.settle();
    CHECK_EQ(static_cast<int>(r.app.screen()), screen_before);
  }

  CASE("brightness is reached through the menu and by no shortcut");
  {
    // Deliberately the only way in. A modifier gesture that adjusted it from
    // anywhere competed with ordinary turning for the same movement, and on
    // this hardware it lost: the push switch shares its ground with the rotary
    // contacts, so turning dipped the switch line and every turn was read as
    // the modifier.
    AppRig r;
    r.tap(2); r.settle();  // the clock
    const int before = r.app.state().brightness;
    r.ports.raw.encoder_sw = true;
    r.run(0.15f);
    r.turn(4);
    r.ports.raw.encoder_sw = false;
    r.settle();
    CHECK_EQ(r.app.state().brightness, before);  // nothing happened to it

    r.enter("DIM");
    r.turn(4);
    r.settle();
    CHECK(r.app.state().brightness > before);  // and here it does
  }
}

void test_app_sleep_and_settings() {
  CASE("the left and right pads together sleep it, and anything wakes it");
  {
    AppRig r;
    r.chord(0, 2); r.settle();
    CHECK(r.app.asleep());
    CHECK_EQ(static_cast<int>(r.app.screen()), static_cast<int>(panel::Screen::Sleep));
    r.tap(1); r.settle();
    CHECK(!r.app.asleep());
  }

  CASE("the gesture that wakes it is swallowed");
  {
    // Reaching for a sleeping panel must not also change what it is saying.
    AppRig r;
    r.chord(0, 2); r.settle();
    const int status_before = static_cast<int>(r.app.state().status);
    r.tap(0); r.settle();
    CHECK(!r.app.asleep());
    CHECK_EQ(static_cast<int>(r.app.state().status), status_before);
  }

  CASE("laying it flat sleeps it and standing it up wakes it");
  {
    AppRig r;
    r.ports.raw.motion_valid = true;
    r.ports.raw.ax = 0.0f; r.ports.raw.ay = 1.0f; r.ports.raw.az = 0.0f;
    r.run(2.0f);
    r.ports.raw.ay = 0.0f; r.ports.raw.az = 1.0f;  // laid down
    r.run(1.5f);
    CHECK(r.app.asleep());
    r.ports.raw.ay = 1.0f; r.ports.raw.az = 0.0f;  // stood back up
    r.run(1.5f);
    CHECK(!r.app.asleep());
  }

  CASE("settings are written once a knob stops moving, not once per detent");
  {
    AppRig r;
    r.enter("DIM");
    r.ports.saves = 0;
    for (int i = 0; i < 20; ++i) r.turn(1);  // a sweep
    CHECK_EQ(r.ports.saves, 0);              // nothing yet
    CHECK(r.app.save_pending());
    r.run(2.0f);
    CHECK_EQ(r.ports.saves, 1);              // exactly one write for the sweep
    CHECK(!r.app.save_pending());
  }

  CASE("what was saved comes back on the next boot");
  {
    AppRig r;
    r.enter("DIM");
    for (int i = 0; i < 10; ++i) r.turn(1);
    r.run(2.0f);
    const int saved = r.app.state().brightness;

    App again;
    again.begin(r.ports, 0.0);
    CHECK_EQ(again.state().brightness, saved);
  }

  CASE("a blob from the future is refused rather than reinterpreted");
  {
    TestPorts p;
    p.stored.version = Settings::kVersion + 7;
    p.stored.brightness = 200;
    p.valid = true;
    App a;
    a.begin(p, 0.0);
    CHECK_EQ(a.state().brightness, Settings{}.brightness);  // defaults, not 200
  }

  CASE("a corrupt blob is clamped into something usable");
  {
    TestPorts p;
    p.stored.version = Settings::kVersion;
    p.stored.brightness = 0;      // a panel that looks dead
    p.stored.work_min = 0;        // a timer that ends instantly
    p.stored.hue = 40.0f;         // nonsense
    p.valid = true;
    App a;
    a.begin(p, 0.0);
    CHECK(a.state().brightness >= 4);
    CHECK(a.settings().work_min >= 1);
    CHECK(a.state().hue >= 0.0f && a.state().hue <= 1.0f);
  }

  CASE("locking the pads silences them but leaves the knob alone");
  {
    AppRig r;
    Settings s = r.app.settings();
    s.touch_locked = true;
    r.ports.stored = s;
    r.ports.valid = true;
    App a;
    a.begin(r.ports, 0.0);
    double t = 0.0;
    const int boot = static_cast<int>((panel::kBootSeconds + 0.8f) / kTick);
    for (int i = 0; i < boot; ++i) { a.update(kTick, t); t += kTick; }

    r.ports.raw.touch[0] = true;
    for (int i = 0; i < 12; ++i) { a.update(kTick, t); t += kTick; }
    r.ports.raw.touch[0] = false;
    for (int i = 0; i < 12; ++i) { a.update(kTick, t); t += kTick; }
    CHECK_EQ(static_cast<int>(a.state().status), static_cast<int>(panel::Status::Free));

    r.ports.raw.encoder_detents += 1;
    for (int i = 0; i < 200; ++i) { a.update(kTick, t); t += kTick; }
    CHECK_EQ(static_cast<int>(a.screen()), static_cast<int>(panel::Screen::Clock));
  }
}


void test_app_menu() {
  CASE("the knob scrolls the menu, and it wraps");
  {
    AppRig r;
    r.open_menu();
    CHECK_EQ(r.app.state().menu_index, 0);
    r.turn(1); r.settle();
    CHECK_EQ(r.app.state().menu_index, 1);
    for (int i = 0; i < panel::kMenuCount - 1; ++i) { r.turn(1); r.settle(); }
    CHECK_EQ(r.app.state().menu_index, 0);   // all the way round
    r.turn(-1); r.settle();
    CHECK_EQ(r.app.state().menu_index, panel::kMenuCount - 1);
  }

  CASE("STAT opens the picker on the status you are actually showing");
  {
    AppRig r;
    r.tap(0); r.settle();  // go BUSY first
    r.enter("STAT");
    CHECK_EQ(static_cast<int>(r.app.screen()),
             static_cast<int>(panel::Screen::StatusPick));
    CHECK_EQ(static_cast<int>(r.app.state().pick),
             static_cast<int>(panel::Status::Busy));
  }

  CASE("scrolling the picker previews without committing");
  {
    AppRig r;
    r.enter("STAT");
    const int shown = static_cast<int>(r.app.state().status);
    r.turn(1); r.settle();
    r.turn(1); r.settle();
    CHECK(static_cast<int>(r.app.state().pick) != shown);
    CHECK_EQ(static_cast<int>(r.app.state().status), shown);  // not yet
  }

  CASE("pressing in the picker commits it and returns home");
  {
    AppRig r;
    r.enter("STAT");
    r.turn(1); r.settle();
    r.turn(1); r.settle();
    const int picked = static_cast<int>(r.app.state().pick);
    r.press(); r.settle();
    CHECK_EQ(static_cast<int>(r.app.state().status), picked);
    CHECK_EQ(r.app.depth(), 1);
  }

  CASE("every status in the picker can be reached by turning");
  {
    // The picker walks the whole enum, badge layouts and icon layouts alike,
    // so a status that cannot be selected cannot be added by accident.
    AppRig r;
    r.enter("STAT");
    bool seen[static_cast<int>(panel::Status::Count)] = {false};
    for (int i = 0; i < static_cast<int>(panel::Status::Count) + 1; ++i) {
      seen[static_cast<int>(r.app.state().pick)] = true;
      r.turn(1);
      r.settle();
    }
    for (int i = 0; i < static_cast<int>(panel::Status::Count); ++i) CHECK(seen[i]);
  }

}


void test_app_flourish() {
  CASE("the timer reaching zero takes the panel");
  {
    AppRig r;
    // Dial the timer down to one minute so the test does not run for 25.
    r.enter("TIME");
    for (int i = 0; i < 40; ++i) r.turn(-1);
    r.settle();
    CHECK_EQ(r.app.state().timer_set_min, 1);
    r.app.handle(Event(EventType::DoublePress), 0.0);
    r.settle();

    r.tap(1);  // start it
    CHECK(r.app.state().timer_running);
    r.run_until_timer_ends();
    CHECK_EQ(r.app.state().timer_left_s, 0);
    CHECK(!r.app.state().timer_running);
    CHECK_EQ(static_cast<int>(r.app.flourish()),
             static_cast<int>(panel::FlourishKind::Done));
  }

  CASE("and it clears itself without being touched");
  {
    AppRig r;
    r.enter("TIME");
    for (int i = 0; i < 40; ++i) r.turn(-1);
    r.settle();
    r.app.handle(Event(EventType::DoublePress), 0.0);
    r.settle();
    r.tap(1);
    r.run_until_timer_ends();
    CHECK_EQ(static_cast<int>(r.app.flourish()),
             static_cast<int>(panel::FlourishKind::Done));
    r.run(panel::kDoneSeconds + 0.2f);
    CHECK_EQ(static_cast<int>(r.app.flourish()),
             static_cast<int>(panel::FlourishKind::None));
  }

  CASE("a celebration can be got out of");
  {
    // A moment you have to sit through is an obstacle, not a flourish.
    AppRig r;
    r.enter("TIME");
    for (int i = 0; i < 40; ++i) r.turn(-1);
    r.settle();
    r.app.handle(Event(EventType::DoublePress), 0.0);
    r.settle();
    r.tap(1);
    r.run_until_timer_ends();
    CHECK_EQ(static_cast<int>(r.app.flourish()),
             static_cast<int>(panel::FlourishKind::Done));
    r.tap(2);
    CHECK_EQ(static_cast<int>(r.app.flourish()),
             static_cast<int>(panel::FlourishKind::None));
  }

  CASE("a fast turn moves a list by its detents, not by one per frame");
  {
    // Spinning three clicks should land three entries on, in one movement.
    // Applying one step per frame instead started and abandoned a transition
    // every ten milliseconds, which is what made a fast turn feel like the
    // panel was fighting itself.
    AppRig r;
    r.open_menu();
    CHECK_EQ(r.app.state().menu_index, 0);
    r.turn(3);
    r.settle();
    CHECK_EQ(r.app.state().menu_index, 3 % panel::kMenuCount);
  }

  CASE("and a fast turn on the carousel does not skip past where you aimed");
  {
    AppRig r;
    CHECK_EQ(static_cast<int>(r.app.screen()), static_cast<int>(panel::Screen::Status));
    r.turn(2);
    r.settle();
    CHECK_EQ(static_cast<int>(r.app.screen()), static_cast<int>(panel::Screen::Timer));
  }

  CASE("a stopped timer says it can be started");
  {
    // The one piece of motion on that screen that is about what you could do
    // rather than what is happening.
    AppRig r;
    r.tap(2); r.settle();
    r.tap(2); r.settle();  // to the timer
    CHECK_EQ(static_cast<int>(r.app.screen()), static_cast<int>(panel::Screen::Timer));
    CHECK(!r.app.state().timer_running);

    // Rows 1..5 in the first three columns: where the arrow lives. Row 7 is
    // the progress rail, which reaches column 0 whatever the timer is doing.
    auto arrow_ink = [](const panel::Framebuffer& f) {
      int n = 0;
      for (int y = 1; y <= 5; ++y)
        for (int x = 0; x < 3; ++x)
          if (f.get(x, y).lit()) ++n;
      return n;
    };

    panel::Framebuffer stopped, running;
    r.render(stopped);
    CHECK(arrow_ink(stopped) > 0);

    r.tap(1);  // start it
    r.settle();
    r.render(running);
    CHECK_EQ(arrow_ink(running), 0);  // and gone once it is running
  }
}


void test_view_transition() {
  CASE("a turn mid-transition retargets rather than restarting it");
  {
    // A detent arrives about every 150 ms and the disk takes 420, so asking for
    // a new screen on each one restarts the movement before it has played a
    // third of itself. The shear that makes it read as a disk never appears and
    // the panel looks like it is jumping between views.
    AppRig r;
    r.turn(1);
    r.run(0.10f);
    CHECK(r.app.busy());
    const float p1 = 0.10f / panel::kDiskSeconds;

    r.turn(1);          // a second detent, part way through
    r.run(0.02f);
    CHECK(r.app.busy());  // still the same movement, not a new one
    // Had it restarted, progress would have fallen back toward zero.
    CHECK(r.app.transition_progress() > p1);

    r.settle();
    CHECK(!r.app.busy());
    CHECK_EQ(static_cast<int>(r.app.screen()), static_cast<int>(panel::Screen::Timer));
  }

  CASE("and it lands on the view the detents asked for");
  {
    AppRig r;
    r.turn(2);
    r.settle();
    CHECK_EQ(static_cast<int>(r.app.screen()), static_cast<int>(panel::Screen::Timer));
    r.turn(-2);
    r.settle();
    CHECK_EQ(static_cast<int>(r.app.screen()), static_cast<int>(panel::Screen::Status));
  }
}

void test_nav_inverse() {
  CASE("every transition that has a direction knows how to come back");
  {
    using panel::TransitionKind;
    CHECK_EQ(static_cast<int>(inverse_of(TransitionKind::WipeUp)),
             static_cast<int>(TransitionKind::WipeDown));
    CHECK_EQ(static_cast<int>(inverse_of(TransitionKind::DiskUp)),
             static_cast<int>(TransitionKind::DiskDown));
    // And inverting twice is the identity, for every kind there is.
    for (int k = 0; k < static_cast<int>(TransitionKind::Count); ++k) {
      const auto kind = static_cast<TransitionKind>(k);
      CHECK_EQ(static_cast<int>(inverse_of(inverse_of(kind))), k);
    }
  }
}

}  // namespace

void run_ui_tests() {
  std::printf("input tests\n");
  test_quadrature();
  test_taps();
  test_chord_and_swipe();
  test_chord_is_not_a_swipe();
  test_encoder();
  test_switch_noise();
  test_motion();
  test_no_crosstalk();
  test_app_boot_and_views();
  test_app_status();
  test_app_timer();
  test_app_menu();
  test_app_adjust();
  test_app_flourish();
  test_app_sleep_and_settings();
  test_view_transition();
  test_nav_inverse();
}
