// Host tests for the input layer.
//
// Every one of these drives the recogniser with a synthetic trace at a fixed
// tick and asserts on exactly what came out. That is the whole reason the
// recogniser is free of ESP-IDF: a gesture is nothing but edge cases, and none
// of them are worth discovering with a finger on a soldered board.
#include <cstring>
#include <vector>

#include "harness.h"
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
    Rig r;
    r.sw(true);
    r.hold_for(0.05f);
    r.sw(false);
    r.step();
    CHECK_EQ(r.count(EventType::Press), 1);
    r.sw(true);
    r.hold_for(0.05f);
    r.sw(false);
    r.step();
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
    r.step();
    CHECK_EQ(r.count(EventType::PressHoldEnd), 1);
    CHECK_EQ(r.count(EventType::Press), 0);
  }

  CASE("turning while the knob is down is a modifier, and not a press");
  {
    Rig r;
    r.sw(true);
    r.hold_for(0.05f);
    r.turn(2);
    r.step();
    CHECK_EQ(r.count(EventType::PressTurn), 1);
    CHECK_EQ(r.count(EventType::Turn), 0);
    r.sw(false);
    r.step();
    // Releasing after a press-turn must not also fire a press, or every
    // brightness adjustment would end by activating whatever the press does.
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

}  // namespace

void run_ui_tests() {
  std::printf("input tests\n");
  test_quadrature();
  test_taps();
  test_chord_and_swipe();
  test_chord_is_not_a_swipe();
  test_encoder();
  test_motion();
  test_no_crosstalk();
}
