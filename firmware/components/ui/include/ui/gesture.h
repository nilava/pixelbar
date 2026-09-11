// Turning pin levels into intent.
//
// Fed the raw state every frame, emitting zero or more events. Pure and
// allocation-free, so a test can drive it with a synthetic trace and assert on
// exactly what came out.
//
// The rule that shapes it: a tap must never wait to find out whether it is
// going to become a double tap. Holding a tap back for the 280 ms it takes to
// rule out a second one would put that latency on the most-used gesture on the
// device. Instead the first tap fires immediately and the second *supersedes*
// it — which works because the actions behind them are state assignments
// (BUSY, then CALL) rather than steps. Where an action cannot be superseded the
// double tap is dropped rather than paid for.
#pragma once
#include <cstdint>

#include "ui/event.h"

namespace ui {

// Decodes a quadrature pair into detents.
//
// Not a hardware counter, because the ESP32-C3 has no PCNT peripheral —
// SOC_PCNT_SUPPORTED is defined for the S3 and absent here. On the device this
// runs in a GPIO interrupt on both edges of both lines; on the host a test
// drives it directly. Either way it is the same four-state table.
// Gray-code table: index is (previous << 2) | current, value is the direction.
// Zero is "no movement"; the two entries that would mean both lines changed in
// one sample are marked 2 and counted rather than guessed at.
inline constexpr int8_t kQuadTable[16] = {
    //        now: 00  01  10  11
    /* prev 00 */   0, -1, +1,  2,
    /* prev 01 */  +1,  0,  2, -1,
    /* prev 10 */  -1,  2,  0, +1,
    /* prev 11 */   2, +1, -1,  0,
};

class Quadrature {
 public:
  // One sample of the A and B lines. Returns the change in detents: +1, -1 or
  // 0. A full detent on an EC11 is four quadrature steps.
  //
  // Defined here rather than in a .cpp so the device's GPIO interrupt can
  // inline it into IRAM. An interrupt that lives in flash stalls for the
  // milliseconds the cache is disabled during an NVS commit, and on an encoder
  // that shows up as detents silently going missing. The host tests exercise
  // this same source, so there is no second copy to drift.
  int update(bool a, bool b) {
    const uint8_t now = static_cast<uint8_t>((a ? 2 : 0) | (b ? 1 : 0));
    if (!primed_) {
      primed_ = true;
      state_ = now;
      return 0;
    }
    if (now == state_) return 0;
    const int8_t step = kQuadTable[(state_ << 2) | now];
    state_ = now;
    if (step == 2) {
      ++illegal_;  // both lines moved between samples: the direction is unknown
      return 0;
    }
    sub_ = static_cast<int8_t>(sub_ + step);
    if (sub_ >= 4) {  // four quarter-steps to a detent on an EC11
      sub_ = 0;
      ++detents_;
      return +1;
    }
    if (sub_ <= -4) {
      sub_ = 0;
      --detents_;
      return -1;
    }
    return 0;
  }

  int32_t detents() const { return detents_; }
  // Transitions that cannot happen on a clean signal — both lines changing at
  // once. A non-zero count at normal turning speed means samples are being
  // missed, which is the signal to move the sampling into an interrupt.
  uint32_t illegal() const { return illegal_; }
  void reset();

 private:
  uint8_t state_ = 0;
  bool primed_ = false;
  int8_t sub_ = 0;  // quarter-steps within the current detent
  int32_t detents_ = 0;
  uint32_t illegal_ = 0;
};

// The maximum number of events one frame can produce. A chord plus a hold end
// plus a turn plus a press is already generous; anything beyond this is a test
// driving nonsense.
constexpr int kMaxEventsPerFrame = 8;

class Recogniser {
 public:
  explicit Recogniser(const GestureConfig& cfg = GestureConfig{}) : cfg_(cfg) {}

  void set_config(const GestureConfig& cfg) { cfg_ = cfg; }
  const GestureConfig& config() const { return cfg_; }

  // Advances by dt and writes up to max events. Returns how many.
  int update(const RawInput& in, float dt_s, Event* out, int max);

  void reset();

 private:
  struct ZoneState {
    bool down = false;         // debounced level
    bool raw = false;          // last raw level
    float stable_s = 0.0f;     // how long raw has held its value
    float down_s = 0.0f;       // how long it has been down
    float since_release_s = 0.0f;
    bool hold_fired = false;
    bool tap_pending = false;  // a tap has been emitted and could be doubled
    bool consumed = false;     // part of a chord or swipe: emits no tap
    // Another pad was touched during this one's press. A lone tap never has
    // that, so it costs a single tap nothing — but it is what stops the first
    // pad of a swipe from firing a tap before the swipe is even knowable. The
    // alternative would be holding every tap back until a swipe is ruled out,
    // which is the latency the whole design is arranged to avoid.
    bool multi_seen = false;
  };

  int emit(Event* out, int max, int n, const Event& e);

  GestureConfig cfg_;
  ZoneState z_[kZones];

  // Cross-zone state.
  //
  // Telling a chord from a swipe is the one genuinely hard call here, because
  // a finger dragging across the pads also lights two of them at once. Timing
  // alone cannot separate them: a brisk swipe and a two-finger press look
  // identical for the first hundred milliseconds. What separates them is that a
  // chord is *held* and a drag is not — so a chord is only confirmed once the
  // zones have survived chord_s together, and any zone lifting cancels it.
  //
  // That costs a chord 120 ms of latency, which is the right place to spend it:
  // chords are for sleep and scene, never for the hot path.
  uint8_t down_mask_ = 0;
  uint8_t chord_cand_ = 0;   // the set being timed
  float chord_cand_s_ = 0.0f;
  bool chord_fired_ = false;
  int8_t swipe_seq_[kZones] = {-1, -1, -1};
  int swipe_len_ = 0;
  float swipe_last_s_ = 0.0f;

  // The encoder switch, with the same tap/hold shape as a zone.
  float since_turn_s_ = 10.0f;  // how long since the knob last moved
  bool sw_down_ = false;        // debounced
  bool sw_raw_ = false;         // last raw level
  float sw_stable_s_ = 0.0f; // how long raw has held its value
  float sw_down_s_ = 0.0f;
  float sw_since_release_s_ = 0.0f;
  bool sw_hold_fired_ = false;
  bool sw_press_pending_ = false;
  bool sw_turned_while_down_ = false;

  int32_t last_detents_ = 0;
  float turn_accum_ = 0.0f;  // detents inside the rate window
  float turn_win_s_ = 0.0f;
  float last_rate_ = 0.0f;   // held between windows, so every turn has a rate

  // Motion.
  bool motion_primed_ = false;
  float prev_mag_ = 1.0f;
  float flat_s_ = 0.0f;
  bool is_flat_ = false;
  float shake_lockout_s_ = 0.0f;
  float tap_lockout_s_ = 0.0f;
};

}  // namespace ui
