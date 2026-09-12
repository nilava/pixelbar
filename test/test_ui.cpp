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
#include "panel/mini_font.h"
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

  CASE("an illegal transition is not counted, but does not lose the turn");
  {
    // At speed, both lines appearing to change at once means a missed edge,
    // not noise — the knob is being turned faster than the samples arrive. The
    // step itself cannot be counted because its direction is unknown, but the
    // quarter-steps either side of it are real movement and must survive it.
    // Discarding them drops a detent out of every fast sweep, which is what
    // an unresponsive knob actually feels like.
    Quadrature q;
    q.update(false, false);
    q.update(true, false);   // one quarter
    q.update(true, true);    // two
    q.update(false, true);   // three
    CHECK_EQ(q.detents(), 0);
    q.update(true, false);   // both lines move: illegal, uncounted
    CHECK_EQ(q.illegal(), 1u);
    CHECK_EQ(q.detents(), 0);
    // The three quarters still stand, so the next one completes the detent
    // the user turned.
    CHECK_EQ(q.update(true, true), 1);
    CHECK_EQ(q.detents(), 1);
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

  CASE("a dip that starts a fast turn is not a press");
  {
    // The real failure, reproduced: the switch line dips as the rotary
    // contacts begin to work, but the first detent needs four quadrature edges
    // and has not been decoded yet — so the turn guard is not armed and the
    // dip latches. On the Status screen a press toggles BUSY, so spinning the
    // knob kept setting the status on the way past.
    Rig r;
    r.hold_for(0.5f);           // knob has been still for a long time
    r.sw(true);                 // ground dips as the contacts start moving
    r.hold_for(0.015f);         // long enough to clear switch_stable_s
    r.sw(false);
    r.hold_for(0.02f);
    // Only now does the first detent finish decoding.
    r.turn(1);
    r.hold_for(0.05f);
    CHECK_EQ(r.count(EventType::Press), 0);
    CHECK_EQ(r.count(EventType::Turn), 1);
  }

  CASE("but a deliberate press still registers");
  {
    // The guard is a duration, so it must not eat a real click. 80 ms is a
    // brisk press by any measure.
    Rig r;
    r.sw(true);
    r.hold_for(0.08f);
    r.sw(false);
    r.hold_for(0.04f);  // release debounce has to clear before Press is emitted
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


void test_switch_ignored_while_turning() {
  CASE("a sustained switch dip during a fast turn is not a press");
  {
    // The failure this pins, reported twice from the board: during a fast turn
    // the rotary contacts make and break continuously, so the ground they share
    // with the push switch is disturbed for as long as the turning lasts — not
    // for a few milliseconds. A longer debounce cannot fix that without making
    // a real press feel slow, so the switch is simply not read while the knob
    // is moving.
    Rig r;
    for (int i = 0; i < 60; ++i) {
      r.turn(1);
      r.sw(i % 3 != 0);  // the line dipping and recovering, for many frames
      r.step();
    }
    r.sw(false);
    r.hold_for(0.3f);
    CHECK_EQ(r.count(EventType::Press), 0);
    CHECK_EQ(r.count(EventType::DoublePress), 0);
    CHECK_EQ(r.count(EventType::PressHoldBegin), 0);
    // And every detent still counted.
    int total = 0;
    for (const Event& e : r.got())
      if (e.type == EventType::Turn) total += e.delta;
    CHECK_EQ(total, 60);
  }

  CASE("and holding the switch through a turn does not open the menu");
  {
    // The other half of the same report: the menu kept exiting on its own,
    // because a dip long enough to look like a hold arrived while turning.
    Rig r;
    r.sw(true);
    for (int i = 0; i < 80; ++i) {
      r.turn(1);
      r.step();
    }
    CHECK_EQ(r.count(EventType::PressHoldBegin), 0);
  }

  CASE("a press once the knob has stopped still works, and quickly");
  {
    Rig r;
    r.turn(5);
    r.step();
    r.hold_for(0.12f);  // the settle window, and no more
    r.sw(true);
    r.hold_for(0.05f);
    r.sw(false);
    r.hold_for(0.04f);
    CHECK_EQ(r.count(EventType::Press), 1);
  }

  CASE("and a hold once it has stopped still opens the menu");
  {
    Rig r;
    r.turn(3);
    r.step();
    r.hold_for(0.12f);
    r.sw(true);
    r.hold_for(0.6f);
    CHECK_EQ(r.count(EventType::PressHoldBegin), 1);
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
  ui::Ports::NetMode net_mode() override { return mode; }
  int64_t unix_time() override { return has_clock ? epoch : 0; }
  bool take_draw(ui::DrawPayload* out) override {
    if (!draw_pending) return false;
    draw_pending = false;
    *out = draw;
    return true;
  }

  ui::DrawPayload draw;
  bool draw_pending = false;
  int64_t epoch = 1789000000;
  const char* net_text() override { return text; }

  ui::Ports::NetMode mode = ui::Ports::NetMode::Online;
  const char* text = "";

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
  // `with_clock` false starts the rig with no time source at all, which is
  // what a real device does until SNTP answers. time_valid latches on and
  // never off, so a test about the unsynced state has to begin there.
  explicit AppRig(bool with_clock = true,
                  ui::Ports::NetMode net = ui::Ports::NetMode::Online) {
    ports.has_clock = with_clock;
    ports.mode = net;
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
  // A deliberate click. Measured presses run 80-150 ms; the recogniser
  // rejects anything under GestureConfig::min_press_s as contact bounce.
  void press(float seconds = 0.09f) {
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
  // Scrolls the list that is currently on screen to the named entry. Reads the
  // labels out of UiState rather than out of a table, so it works for both
  // levels of the tree without being told which one it is looking at.
  void list_to(const char* label) {
    const int n = app.state().list_count;
    for (int guard = 0; guard <= n; ++guard) {
      const int i = app.state().menu_index;
      if (i >= 0 && i < n && app.state().list &&
          std::strcmp(app.state().list[i].label, label) == 0)
        return;
      turn(1);
      settle();
    }
  }
  void menu_to(const char* label) { list_to(label); }
  // Group, then item. A one-argument call opens a shortcut group.
  void enter(const char* group, const char* item = nullptr) {
    open_menu();
    list_to(group);
    press();
    settle();
    if (item) {
      list_to(item);
      press();
      settle();
    }
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
    CHECK_EQ(static_cast<int>(r.app.screen()), static_cast<int>(panel::Screen::Scene));
    r.tap(2); r.settle();
    CHECK_EQ(static_cast<int>(r.app.screen()), static_cast<int>(panel::Screen::Status));

    r.turn(1); r.settle();
    CHECK_EQ(static_cast<int>(r.app.screen()), static_cast<int>(panel::Screen::Clock));
    r.turn(-1); r.settle();
    CHECK_EQ(static_cast<int>(r.app.screen()), static_cast<int>(panel::Screen::Status));
    r.turn(-1); r.settle();  // wrapping backwards
    CHECK_EQ(static_cast<int>(r.app.screen()), static_cast<int>(panel::Screen::Scene));

    // Every home view is walked, in order, and the list closes on itself.
    // Written against kHomeViews rather than a copy of it, so adding a view
    // extends the test instead of quietly escaping it.
    AppRig q;
    for (int i = 0; i < kHomeViewCount; ++i) {
      CHECK_EQ(static_cast<int>(q.app.screen()), static_cast<int>(kHomeViews[i]));
      q.turn(1);
      q.settle();
    }
    CHECK_EQ(static_cast<int>(q.app.screen()), static_cast<int>(kHomeViews[0]));
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

  CASE("a status change brings the status view with it");
  {
    // This used to assert the opposite — that changing a status from the clock
    // left you on the clock — on the reasoning that the panel had not moved
    // you anywhere, only one fact about it had changed.
    //
    // That reasoning was about the person holding the device, and the audience
    // is the room. A status animating over a clock face leaves the room
    // looking at a clock, which is the one thing it certainly does not need to
    // be told. Reversed deliberately.
    AppRig r;
    r.tap(2); r.settle();  // go to the clock
    CHECK_EQ(static_cast<int>(r.app.screen()), static_cast<int>(panel::Screen::Clock));
    r.tap(0); r.settle();  // change status from there
    CHECK_EQ(static_cast<int>(r.app.state().status), static_cast<int>(panel::Status::Busy));
    CHECK_EQ(static_cast<int>(r.app.screen()), static_cast<int>(panel::Screen::Status));
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

    r.list_to("DISP");
    r.press(); r.settle();
    CHECK_EQ(r.app.depth(), 3);
    CHECK_EQ(static_cast<int>(r.app.screen()), static_cast<int>(panel::Screen::Group));

    r.list_to("DIM");
    r.press(); r.settle();
    CHECK_EQ(r.app.depth(), 4);
    CHECK_EQ(static_cast<int>(r.app.screen()), static_cast<int>(panel::Screen::Brightness));

    // Pressing accepts and returns to the list you came from, rather than
    // stepping on to some unrelated setting.
    r.press(); r.settle();
    CHECK_EQ(r.app.depth(), 3);
    CHECK_EQ(static_cast<int>(r.app.screen()), static_cast<int>(panel::Screen::Group));

    // And holding backs out a level at a time, the same way it went in.
    r.press(0.7f); r.settle();
    CHECK_EQ(r.app.depth(), 2);
    CHECK_EQ(static_cast<int>(r.app.screen()), static_cast<int>(panel::Screen::Menu));

    r.press(0.7f); r.settle();
    CHECK_EQ(r.app.depth(), 1);
    CHECK_EQ(static_cast<int>(r.app.screen()), static_cast<int>(panel::Screen::Status));
  }

  CASE("every group leads somewhere, and every setting is reachable");
  {
    // The whole tree, walked. The point is coverage rather than any one path:
    // a setting added to the table with no way in, or a group whose shortcut
    // points at nothing, fails here rather than on the bench.
    for (int g = 0; g < ui::kGroupCount; ++g) {
      const ui::SettingGroup& grp = ui::kGroups[g];
      AppRig r;
      r.enter(grp.row.label);
      if (grp.direct != panel::Screen::Count) {
        CHECK_EQ(static_cast<int>(r.app.screen()), static_cast<int>(grp.direct));
        continue;
      }
      CHECK_EQ(static_cast<int>(r.app.screen()), static_cast<int>(panel::Screen::Group));
      CHECK_EQ(r.app.state().list_count, static_cast<int>(grp.count));
      for (int i = 0; i < grp.count; ++i) {
        const ui::SettingDesc& d = grp.items[i];
        AppRig q;
        q.enter(grp.row.label, d.row.label);
        const panel::Screen want = d.kind == ui::SettingKind::Screen
                                       ? d.screen
                                       : panel::Screen::Setting;
        CHECK_EQ(static_cast<int>(q.app.screen()), static_cast<int>(want));
        // And back out again to where it came from.
        q.press();
        q.settle();
        CHECK_EQ(static_cast<int>(q.app.screen()), static_cast<int>(panel::Screen::Group));
      }
    }
  }

  CASE("claiming a status from the menu shows the status, not the menu");
  {
    // The one deliberate exception to "pressing accepts and steps back".
    // Pressing here claims a status, and the result of that is the
    // room-facing screen — showing the menu again would hide what you just did.
    AppRig r;
    r.enter("STAT");
    CHECK_EQ(static_cast<int>(r.app.screen()), static_cast<int>(panel::Screen::StatusPick));
    r.press();
    r.settle();
    CHECK_EQ(static_cast<int>(r.app.screen()), static_cast<int>(panel::Screen::Status));
    CHECK_EQ(r.app.depth(), 1);
  }

  CASE("a status arriving from elsewhere brings the status view with it");
  {
    // The room is the audience. A status change that animates over the clock
    // leaves the room looking at a clock.
    AppRig r;
    r.turn(1);
    r.settle();
    CHECK(r.app.screen() != panel::Screen::Status);

    r.app.set_status_external(static_cast<int>(panel::Status::Busy));
    r.settle();
    CHECK_EQ(static_cast<int>(r.app.screen()), static_cast<int>(panel::Screen::Status));
    CHECK_EQ(static_cast<int>(r.app.state().status), static_cast<int>(panel::Status::Busy));
  }

  CASE("but it does not drag you out of the menu to do it");
  {
    // A microphone opening mid-settings should not lose your place. The status
    // is correct either way, and you will see it when you come out.
    AppRig r;
    r.enter("DISP", "DIM");
    const int depth = r.app.depth();
    CHECK(depth > 1);

    r.app.set_status_external(static_cast<int>(panel::Status::Call));
    r.settle();
    CHECK_EQ(r.app.depth(), depth);
    CHECK_EQ(static_cast<int>(r.app.screen()), static_cast<int>(panel::Screen::Brightness));
    CHECK_EQ(static_cast<int>(r.app.state().status), static_cast<int>(panel::Status::Call));
  }

  CASE("and it shows the status even if you were looking at something else");
  {
    // This passed for the wrong reason: the rig starts on the status view, so
    // "go home" happened to land there. Turn the knob away first and the bug
    // appears — go_home() returns to whichever home view you last chose, which
    // after claiming a status is the one screen guaranteed not to show it.
    AppRig r;
    r.turn(1);
    r.settle();
    CHECK(r.app.screen() != panel::Screen::Status);   // on the clock now

    r.enter("STAT");
    r.turn(1);                                        // pick something else
    r.settle();
    const panel::Status chosen = r.app.state().pick;
    r.press();
    r.settle();
    CHECK_EQ(static_cast<int>(r.app.screen()), static_cast<int>(panel::Screen::Status));
    CHECK_EQ(static_cast<int>(r.app.state().status), static_cast<int>(chosen));

    // And the carousel agrees with what is on screen: turning once from here
    // goes to the next view, not back to the status you are already on.
    r.turn(1);
    r.settle();
    CHECK(r.app.screen() != panel::Screen::Status);
  }

  CASE("turning on the brightness screen changes brightness, and clamps");
  {
    AppRig r;
    r.enter("DISP", "DIM");
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
    r.enter("DISP", "HUE");
    const panel::RGB before = r.app.state().accent;
    r.turn(6); r.settle();
    const panel::RGB after = r.app.state().accent;
    CHECK(!(before == after));
    CHECK(after == panel::accent_from_hue(r.app.state().hue));
  }

  CASE("a shake backs out of an adjuster but does nothing at home");
  {
    AppRig r;
    r.enter("DISP", "DIM");
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

    r.enter("DISP", "DIM");
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
    r.enter("DISP", "DIM");
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
    r.enter("DISP", "DIM");
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
    for (int i = 0; i < ui::kGroupCount - 1; ++i) { r.turn(1); r.settle(); }
    CHECK_EQ(r.app.state().menu_index, 0);   // all the way round
    r.turn(-1); r.settle();
    CHECK_EQ(r.app.state().menu_index, ui::kGroupCount - 1);
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
  // A one-round, one-minute set: the whole thing ends at the first zero, which
  // is what these three cases are about. Without trimming the rounds the first
  // phase to run out is a *rest*, not the end — see test_timer_cycles.
  auto one_minute_set = [](AppRig& r) {
    r.enter("TIME", "SETS");
    for (int i = 0; i < 20; ++i) r.turn(-1);   // cycles down to 1
    r.settle();
    r.app.handle(Event(EventType::DoublePress), 0.0);
    r.settle();
    r.enter("TIME", "TASK");
    for (int i = 0; i < 40; ++i) r.turn(-1);   // work down to 1 minute
    r.settle();
    r.app.handle(Event(EventType::DoublePress), 0.0);
    r.settle();
  };

  CASE("the timer reaching zero takes the panel");
  {
    AppRig r;
    one_minute_set(r);
    CHECK_EQ(r.app.state().timer_set_min, 1);
    CHECK_EQ(static_cast<int>(r.app.state().timer_cycles), 1);

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
    one_minute_set(r);
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
    one_minute_set(r);
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
    CHECK_EQ(r.app.state().menu_index, 3 % ui::kGroupCount);
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


void test_draw_requests() {
  // A helper that pushes a payload the way a host would.
  auto push = [](AppRig& r, const char* text, uint8_t prio, float ttl,
                 const char* source = "test") {
    ui::DrawPayload p;
    snprintf(p.text, sizeof(p.text), "%s", text);
    snprintf(p.source, sizeof(p.source), "%s", source);
    p.priority = prio;
    p.ttl_s = ttl;
    r.ports.draw = p;
    r.ports.draw_pending = true;
    r.run(0.1f);
  };

  CASE("a request takes the panel and gives it back when it expires");
  {
    AppRig r;
    const panel::Screen home = r.app.screen();
    push(r, "BUILD OK", App::kDrawNotify, 2.0f);
    CHECK_EQ(static_cast<int>(r.app.screen()), static_cast<int>(panel::Screen::Draw));
    CHECK(std::strcmp(r.app.state().draw_text, "BUILD OK") == 0);

    r.run(2.5f);
    r.settle();
    CHECK(r.app.screen() != panel::Screen::Draw);
    CHECK_EQ(static_cast<int>(r.app.screen()), static_cast<int>(home));
  }

  CASE("a more important request replaces a less important one");
  {
    AppRig r;
    push(r, "AMBIENT", App::kDrawAmbient, 30.0f);
    CHECK(std::strcmp(r.app.state().draw_text, "AMBIENT") == 0);
    push(r, "URGENT", App::kDrawUrgent, 30.0f);
    CHECK(std::strcmp(r.app.state().draw_text, "URGENT") == 0);
  }

  CASE("a less important one does not interrupt what is already up");
  {
    AppRig r;
    push(r, "URGENT", App::kDrawUrgent, 30.0f);
    push(r, "AMBIENT", App::kDrawAmbient, 30.0f);
    // Still the important one. A build notification should not push a call
    // off the panel.
    CHECK(std::strcmp(r.app.state().draw_text, "URGENT") == 0);
  }

  CASE("equal priority from anywhere replaces, because it is newer");
  {
    AppRig r;
    push(r, "FIRST", App::kDrawNotify, 30.0f, "a");
    push(r, "SECOND", App::kDrawNotify, 30.0f, "b");
    CHECK(std::strcmp(r.app.state().draw_text, "SECOND") == 0);
  }

  CASE("a press puts the panel back");
  {
    AppRig r;
    push(r, "MESSAGE", App::kDrawNotify, 60.0f);
    CHECK_EQ(static_cast<int>(r.app.screen()), static_cast<int>(panel::Screen::Draw));
    r.press();
    r.settle();
    CHECK(r.app.screen() != panel::Screen::Draw);
  }

  CASE("a countdown is worked out from the deadline, not sent as a number");
  {
    // The host sends a moment; the panel works out how far away it is. A
    // duration would already be stale by however long the request took to
    // arrive, and this one sits on screen for minutes.
    AppRig r;
    ui::DrawPayload p;
    snprintf(p.text, sizeof(p.text), "STANDUP");
    p.priority = App::kDrawUrgent;
    p.ttl_s = 5.0f;
    p.until_unix = r.ports.epoch + 300;      // five minutes out
    r.ports.draw = p;
    r.ports.draw_pending = true;
    r.run(0.1f);
    CHECK_EQ(r.app.state().draw_seconds, 300);

    // Time passes on the host's clock; the panel follows it.
    r.ports.epoch += 120;
    r.run(0.1f);
    CHECK_EQ(r.app.state().draw_seconds, 180);

    // And it outlives its ttl, because something counting down to a moment
    // should not vanish before the moment arrives.
    r.run(10.0f);
    CHECK_EQ(static_cast<int>(r.app.screen()), static_cast<int>(panel::Screen::Draw));
  }

  CASE("with no clock there is no countdown, rather than a wrong one");
  {
    AppRig r(false);              // no time source
    ui::DrawPayload p;
    snprintf(p.text, sizeof(p.text), "SOON");
    p.priority = App::kDrawNotify;
    p.ttl_s = 30.0f;
    p.until_unix = 1789000300;
    r.ports.draw = p;
    r.ports.draw_pending = true;
    r.run(0.1f);
    CHECK_EQ(r.app.state().draw_seconds, -1);
  }

  CASE("an icon is named, and an unknown name is simply no icon");
  {
    AppRig r;
    ui::DrawPayload p;
    snprintf(p.text, sizeof(p.text), "HI");
    snprintf(p.icon, sizeof(p.icon), "WARNING");   // case-folded on purpose
    p.priority = App::kDrawNotify;
    p.ttl_s = 30.0f;
    r.ports.draw = p;
    r.ports.draw_pending = true;
    r.run(0.1f);
    CHECK(r.app.state().draw_icon != nullptr);

    snprintf(p.icon, sizeof(p.icon), "nosuchicon");
    r.ports.draw = p;
    r.ports.draw_pending = true;
    r.run(0.1f);
    // A host naming an icon this firmware does not have is version skew, not
    // a fault: it draws the rest of the message.
    CHECK(r.app.state().draw_icon == nullptr);
    CHECK(std::strcmp(r.app.state().draw_text, "HI") == 0);
  }
}

void test_timer_cycles() {
  // Dials a set down to one-minute phases so a whole pomodoro runs inside a
  // test. Each turn is settled before the next: the adjuster multiplies the
  // step once the measured turn rate is high, so a burst of twenty detents
  // followed immediately by one more moves by five, not by one.
  auto dial = [](AppRig& r, const char* item, int downs, int ups) {
    r.enter("TIME", item);
    for (int i = 0; i < downs; ++i) r.turn(-1);
    r.settle();
    for (int i = 0; i < ups; ++i) { r.turn(1); r.settle(); }
    r.app.handle(Event(EventType::DoublePress), 0.0);
    r.settle();
  };
  auto short_set = [&](AppRig& r, int rounds) {
    dial(r, "SETS", 20, rounds - 1);   // cycles: floor is 1
    dial(r, "REST", 80, 0);            // rest: floor is 1 minute
    dial(r, "TASK", 40, 0);            // work: floor is 1 minute
  };
  // One phase, plus a second to cross the boundary. run_until_timer_ends is no
  // use here: at a boundary the next phase is loaded in the same frame, so the
  // countdown is never observed at zero and that helper would run through the
  // whole set.
  constexpr float kPhase = 61.0f;

  CASE("work runs into a rest, and rest into the next round");
  {
    // The settings for this have existed since the settings tree landed and
    // were read by nothing: the timer counted work down once and stopped.
    AppRig r;
    short_set(r, 2);
    CHECK_EQ(static_cast<int>(r.app.state().timer_cycles), 2);
    CHECK_EQ(static_cast<int>(r.app.state().timer_cycle), 1);
    CHECK(!r.app.state().timer_resting);
    CHECK_EQ(r.app.state().timer_total_s, 60);

    r.tap(1);
    CHECK(r.app.state().timer_running);
    r.run(kPhase);
    // The first zero is a boundary, not an ending.
    CHECK(r.app.state().timer_resting);
    CHECK_EQ(static_cast<int>(r.app.state().timer_cycle), 1);
    CHECK(r.app.flourish() != panel::FlourishKind::Done);
    CHECK(r.app.state().timer_running);   // auto_next defaults on

    r.run(kPhase);
    CHECK(!r.app.state().timer_resting);
    CHECK_EQ(static_cast<int>(r.app.state().timer_cycle), 2);

    // Stepped rather than run flat out, and stopped the moment the timer does.
    // Each boundary costs about a second of drift, so three phases of kPhase
    // overshoot the end by enough for the 1.6 s celebration to have expired
    // before it is asserted on.
    for (int i = 0; i < 200 && r.app.state().timer_running; ++i) r.run(0.5f);
    // The last round ends the set, and there is no rest after it — a rest you
    // are not coming back from is a countdown between you and being finished.
    CHECK(!r.app.state().timer_running);
    CHECK(!r.app.state().timer_resting);
    CHECK_EQ(static_cast<int>(r.app.flourish()),
             static_cast<int>(panel::FlourishKind::Done));
  }

  CASE("with auto-continue off it waits at the boundary");
  {
    AppRig r;
    short_set(r, 2);
    r.enter("TIME", "AUTO");
    r.turn(1);
    r.settle();
    CHECK(!r.app.settings().auto_next);
    r.app.handle(Event(EventType::DoublePress), 0.0);
    r.settle();

    r.tap(1);
    r.run(kPhase);
    CHECK(r.app.state().timer_resting);
    CHECK(!r.app.state().timer_running);   // holding the rest, waiting
    r.tap(1);
    CHECK(r.app.state().timer_running);
  }

  CASE("resetting puts the whole set back, not just the phase");
  {
    AppRig r;
    short_set(r, 2);
    r.tap(1);
    r.run(kPhase);
    CHECK(r.app.state().timer_resting);

    r.hold(1);   // middle zone, hold
    r.settle();
    CHECK(!r.app.state().timer_resting);
    CHECK_EQ(static_cast<int>(r.app.state().timer_cycle), 1);
    CHECK(!r.app.state().timer_running);
    CHECK_EQ(r.app.state().timer_left_s, r.app.state().timer_total_s);
  }

  CASE("dialling the rest length while resting changes the rest on screen");
  {
    // apply_settings retargeted from work_min whatever the phase, so changing
    // any setting while resting would have replaced the rest with a work run.
    AppRig r;
    short_set(r, 2);
    r.tap(1);
    r.run(kPhase);
    CHECK(r.app.state().timer_resting);
    r.tap(1);                              // pause, so a retarget is allowed
    CHECK(!r.app.state().timer_running);

    r.enter("TIME", "REST");
    r.turn(1);
    r.settle();
    CHECK_EQ(r.app.state().timer_total_s,
             static_cast<int>(r.app.settings().rest_min) * 60);
  }
}

void test_settings_tree() {
  CASE("every label fits the box it is drawn in");
  {
    // Measured, not eyeballed. A label wider than fifteen columns is clipped
    // beside its icon, and the only way to know is to measure — M and W are
    // five columns in this font, so four characters is not the rule.
    for (int g = 0; g < ui::kGroupCount; ++g) {
      CHECK(panel::mini_text_fits(ui::kGroups[g].row.label));
      for (int i = 0; i < ui::kGroups[g].count; ++i)
        CHECK(panel::mini_text_fits(ui::kGroups[g].items[i].row.label));
    }
  }

  CASE("every setting round-trips through get and set");
  {
    // The two switches are written by hand and the compiler only checks that
    // every case exists, not that they agree. A setting whose get reads one
    // field and whose set writes another would be invisible until someone
    // changed it and it did nothing.
    for (int g = 0; g < ui::kGroupCount; ++g) {
      for (int i = 0; i < ui::kGroups[g].count; ++i) {
        const ui::SettingDesc& d = ui::kGroups[g].items[i];
        // A setting with its own screen keeps its range there, so lo and hi
        // here are placeholders and walking them would mean nothing.
        if (d.kind == ui::SettingKind::Screen) continue;
        ui::Settings s;
        for (int v = d.lo; v <= d.hi; v += (d.step > 0 ? d.step : 1)) {
          ui::setting_set(s, d.id, v);
          CHECK_EQ(ui::setting_get(s, d.id), v);
        }
      }
    }
  }

  CASE("a setting changed on the panel reaches the stored struct and is saved");
  {
    // The whole point of the tree. Before it, these fields were written to
    // NVS, migrated and sanitised, and could not be changed by any gesture.
    AppRig r;
    r.enter("TILT", "FLAT");
    CHECK_EQ(static_cast<int>(r.app.screen()), static_cast<int>(panel::Screen::Setting));
    const bool before = r.app.settings().flat_sleeps;
    r.turn(1);
    r.settle();
    CHECK(r.app.settings().flat_sleeps != before);

    // And it is written out, once the knob stops rather than once per detent.
    r.run(2.0f);
    CHECK(r.ports.saves >= 1);
    CHECK(r.ports.stored.flat_sleeps != before);
  }

  CASE("a number clamps at its ends and a choice wraps round");
  {
    AppRig r;
    r.enter("TIME", "SETS");           // cycles, 1..12
    for (int i = 0; i < 40; ++i) r.turn(1);
    r.settle();
    CHECK_EQ(static_cast<int>(r.app.settings().cycles), 12);
    for (int i = 0; i < 40; ++i) r.turn(-1);
    r.settle();
    CHECK_EQ(static_cast<int>(r.app.settings().cycles), 1);

    AppRig q;
    q.enter("IDLE", "PICK");           // the scene list, four long
    const int first = q.app.settings().scene;
    for (int i = 0; i < panel::scene_count(); ++i) { q.turn(1); q.settle(); }
    CHECK_EQ(static_cast<int>(q.app.settings().scene), first);
  }

  CASE("the value reads as words where a number would be meaningless");
  {
    ui::Settings s;
    char buf[16];
    // A toggle says what it is, not 0 or 1.
    const ui::SettingDesc* flat = nullptr;
    for (int g = 0; g < ui::kGroupCount && !flat; ++g)
      for (int i = 0; i < ui::kGroups[g].count; ++i)
        if (ui::kGroups[g].items[i].id == ui::SettingId::FlatSleeps)
          flat = &ui::kGroups[g].items[i];
    CHECK(flat != nullptr);
    ui::setting_set(s, ui::SettingId::FlatSleeps, 1);
    CHECK(std::strcmp(ui::setting_text(s, *flat, buf, sizeof(buf)), "ON") == 0);
    ui::setting_set(s, ui::SettingId::FlatSleeps, 0);
    CHECK(std::strcmp(ui::setting_text(s, *flat, buf, sizeof(buf)), "OFF") == 0);
    // And a toggle has no position along a line, so it draws no rail.
    CHECK(ui::setting_fraction(s, *flat) < 0.0f);
  }

  CASE("the timer length dialled on the panel survives a power cut");
  {
    // It did not: the TimerSet screen wrote UiState and never the struct, so
    // the length was right until the next boot and then quietly twenty-five.
    AppRig r;
    r.enter("TIME", "TASK");
    for (int i = 0; i < 5; ++i) r.turn(-1);
    r.settle();
    r.run(2.0f);
    const int dialled = r.app.state().timer_set_min;
    CHECK(dialled != 25);
    CHECK_EQ(static_cast<int>(r.ports.stored.work_min), dialled);
  }
}

void test_net_screens() {
  CASE("a device with no credentials says so, once the boot is over");
  {
    AppRig r(true, ui::Ports::NetMode::Setup);
    CHECK_EQ(static_cast<int>(r.app.screen()),
             static_cast<int>(panel::Screen::WifiSetup));
  }

  CASE("a join the user started is reported; the one at boot is not");
  {
    // Boot straight into joining, as a provisioned device does on every power
    // up. That must not put a progress screen in front of the boot sequence.
    AppRig r(true, ui::Ports::NetMode::Joining);
    CHECK(r.app.screen() != panel::Screen::WifiConnecting);
    r.ports.mode = ui::Ports::NetMode::Online;
    r.run(0.3f);
    CHECK(r.app.screen() != panel::Screen::WifiInfo);
  }

  CASE("but a join that follows setup shows connecting, then the address");
  {
    AppRig r(true, ui::Ports::NetMode::Setup);
    CHECK_EQ(static_cast<int>(r.app.screen()),
             static_cast<int>(panel::Screen::WifiSetup));

    r.ports.mode = ui::Ports::NetMode::Joining;   // the form was submitted
    r.run(0.3f);
    CHECK_EQ(static_cast<int>(r.app.screen()),
             static_cast<int>(panel::Screen::WifiConnecting));

    r.ports.mode = ui::Ports::NetMode::Online;    // and it worked
    r.run(0.3f);
    CHECK_EQ(static_cast<int>(r.app.screen()),
             static_cast<int>(panel::Screen::WifiInfo));
  }

  CASE("the address screen can be pressed away, and times out on its own");
  {
    AppRig r(true, ui::Ports::NetMode::Setup);
    r.ports.mode = ui::Ports::NetMode::Joining;
    r.run(0.3f);
    r.ports.mode = ui::Ports::NetMode::Online;
    r.run(0.3f);
    CHECK_EQ(static_cast<int>(r.app.screen()),
             static_cast<int>(panel::Screen::WifiInfo));
    r.press();
    r.settle();
    CHECK(!App::is_net_screen_public(r.app.screen()));

    // And again, left alone this time.
    AppRig q(true, ui::Ports::NetMode::Setup);
    q.ports.mode = ui::Ports::NetMode::Joining;
    q.run(0.3f);
    q.ports.mode = ui::Ports::NetMode::Online;
    q.run(0.3f);
    CHECK_EQ(static_cast<int>(q.app.screen()),
             static_cast<int>(panel::Screen::WifiInfo));
    q.run(App::kNetInfoSeconds + 1.0f);
    CHECK(!App::is_net_screen_public(q.app.screen()));
  }

  CASE("and turning the knob leaves it too");
  {
    AppRig r(true, ui::Ports::NetMode::Setup);
    CHECK_EQ(static_cast<int>(r.app.screen()),
             static_cast<int>(panel::Screen::WifiSetup));
    r.turn(1);
    r.settle();
    CHECK(!App::is_net_screen_public(r.app.screen()));
  }
}

void test_clock_validity() {
  CASE("the clock is not valid until a time source says so");
  {
    // Before the first sync there is no time. UiState::time_valid staying
    // false is what makes the Clock screen draw dashes instead of 00:00.
    AppRig r(false);
    r.run(0.05f);
    CHECK(!r.app.state().time_valid);
    CHECK_EQ(r.app.state().hour, 0);
    CHECK_EQ(r.app.state().minute, 0);
  }

  CASE("and it latches once the port starts answering");
  {
    AppRig r(false);
    r.run(0.05f);
    CHECK(!r.app.state().time_valid);
    r.ports.has_clock = true;   // SNTP lands
    r.run(0.05f);
    CHECK(r.app.state().time_valid);
    CHECK_EQ(r.app.state().hour, 14);
    CHECK_EQ(r.app.state().minute, 25);
  }
}

void test_view_transition() {
  CASE("a turn mid-transition hands off to a new leg, from the screen arriving");
  {
    // Each detent is its own short movement. The screen that was arriving
    // becomes the screen that leaves, so a knob spun hard shows the first part
    // of each leg in turn rather than blending the view you started on against
    // a destination that keeps moving. Getting this wrong is what made a fast
    // turn look like it had no animation at all.
    AppRig r;
    const panel::Screen first = r.app.screen();
    r.turn(1);
    r.run(0.10f);
    CHECK(r.app.busy());
    CHECK_EQ(static_cast<int>(r.app.transition_from()), static_cast<int>(first));
    const panel::Screen second = r.app.screen();

    r.turn(1);          // a second detent, part way through
    r.run(0.02f);
    CHECK(r.app.busy());
    // The new leg leaves from the screen that had been arriving, not from the
    // one we originally started on.
    CHECK_EQ(static_cast<int>(r.app.transition_from()), static_cast<int>(second));
    // And it is a fresh leg, so there is runway left for it to be seen.
    CHECK(r.app.transition_progress() < 0.5f);

    r.settle();
    CHECK(!r.app.busy());
    CHECK_EQ(static_cast<int>(r.app.screen()), static_cast<int>(panel::Screen::Timer));
  }

  CASE("turning back mid-transition reverses instead of stalling");
  {
    // Forward then straight back used to set the destination to the screen we
    // were already leaving, so the rest of the transition blended a screen
    // against itself and the panel visibly stopped dead.
    AppRig r;
    const panel::Screen home = r.app.screen();
    r.turn(1);
    r.run(0.08f);
    CHECK(r.app.busy());
    const panel::Screen forward = r.app.screen();
    CHECK(forward != home);

    r.turn(-1);
    r.run(0.02f);
    CHECK(r.app.busy());
    // A real movement: two different screens, heading back where we came from.
    CHECK_EQ(static_cast<int>(r.app.screen()), static_cast<int>(home));
    CHECK_EQ(static_cast<int>(r.app.transition_from()), static_cast<int>(forward));
    CHECK(r.app.transition_from() != r.app.screen());

    r.settle();
    CHECK_EQ(static_cast<int>(r.app.screen()), static_cast<int>(home));
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
  test_switch_ignored_while_turning();
  test_motion();
  test_no_crosstalk();
  test_app_boot_and_views();
  test_app_status();
  test_app_timer();
  test_app_menu();
  test_app_adjust();
  test_app_flourish();
  test_app_sleep_and_settings();
  test_draw_requests();
  test_timer_cycles();
  test_settings_tree();
  test_net_screens();
  test_clock_validity();
  test_view_transition();
  test_nav_inverse();
}
