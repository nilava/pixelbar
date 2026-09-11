#include "ui/gesture.h"

#include <cmath>

namespace ui {

const char* event_name(EventType t) {
  switch (t) {
    case EventType::Tap: return "tap";
    case EventType::DoubleTap: return "doubletap";
    case EventType::HoldBegin: return "holdbegin";
    case EventType::HoldEnd: return "holdend";
    case EventType::Chord: return "chord";
    case EventType::Swipe: return "swipe";
    case EventType::Turn: return "turn";
    case EventType::Press: return "press";
    case EventType::DoublePress: return "doublepress";
    case EventType::PressHoldBegin: return "pressholdbegin";
    case EventType::PressHoldEnd: return "pressholdend";
    case EventType::PressTurn: return "pressturn";
    case EventType::CaseTap: return "casetap";
    case EventType::Shake: return "shake";
    case EventType::LaidFlat: return "laidflat";
    case EventType::PickedUp: return "pickedup";
    default: return "none";
  }
}

// ------------------------------------------------------------- quadrature

namespace {

// Gray-code table: index is (previous << 2) | current, value is the direction.
// Zero is "no movement"; the two entries that would mean both lines changed in
// one sample are marked 2 and counted as illegal rather than guessed at.
const int8_t kQuadTable[16] = {
    //        now: 00  01  10  11
    /* prev 00 */   0, -1, +1,  2,
    /* prev 01 */  +1,  0,  2, -1,
    /* prev 10 */  -1,  2,  0, +1,
    /* prev 11 */   2, +1, -1,  0,
};

}  // namespace

int Quadrature::update(bool a, bool b) {
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
    // Both lines moved between samples, so the direction is unknowable.
    ++illegal_;
    return 0;
  }
  sub_ = static_cast<int8_t>(sub_ + step);
  // Four quarter-steps to a detent on an EC11.
  if (sub_ >= 4) {
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

void Quadrature::reset() {
  state_ = 0;
  primed_ = false;
  sub_ = 0;
  detents_ = 0;
  illegal_ = 0;
}

// ------------------------------------------------------------- recogniser

void Recogniser::reset() {
  for (int i = 0; i < kZones; ++i) z_[i] = ZoneState{};
  down_mask_ = 0;
  chord_cand_ = 0;
  chord_cand_s_ = 0.0f;
  chord_fired_ = false;
  for (int i = 0; i < kZones; ++i) swipe_seq_[i] = -1;
  swipe_len_ = 0;
  swipe_last_s_ = 0.0f;
  sw_down_ = false;
  sw_down_s_ = 0.0f;
  sw_since_release_s_ = 0.0f;
  sw_hold_fired_ = false;
  sw_press_pending_ = false;
  sw_turned_while_down_ = false;
  last_detents_ = 0;
  turn_accum_ = 0.0f;
  turn_win_s_ = 0.0f;
  motion_primed_ = false;
  prev_mag_ = 1.0f;
  flat_s_ = 0.0f;
  is_flat_ = false;
  shake_lockout_s_ = 0.0f;
  tap_lockout_s_ = 0.0f;
}

int Recogniser::emit(Event* out, int max, int n, const Event& e) {
  if (n < max) out[n] = e;
  return n + 1 <= max ? n + 1 : max;
}

int Recogniser::update(const RawInput& in, float dt_s, Event* out, int max) {
  int n = 0;
  if (dt_s < 0.0f) dt_s = 0.0f;

  // ---------------------------------------------------------------- touch
  //
  // Debounce first, then work on the settled level only. The TTP223 has its own
  // debounce, so this is rejecting a stray sample rather than a bouncing
  // contact, and the window is short enough not to add felt latency.
  bool went_down[kZones] = {false, false, false};
  bool went_up[kZones] = {false, false, false};

  for (int i = 0; i < kZones; ++i) {
    ZoneState& z = z_[i];
    const bool raw = in.touch[i];
    if (raw != z.raw) {
      z.raw = raw;
      z.stable_s = 0.0f;
    } else {
      z.stable_s += dt_s;
    }
    if (z.stable_s >= cfg_.touch_stable_s && z.down != z.raw) {
      z.down = z.raw;
      if (z.down) {
        went_down[i] = true;
        z.down_s = 0.0f;
        z.hold_fired = false;
        z.consumed = false;
        z.multi_seen = false;
      } else {
        went_up[i] = true;
      }
    }
    if (z.down) {
      z.down_s += dt_s;
    } else {
      z.since_release_s += dt_s;
      if (z.tap_pending && z.since_release_s > cfg_.double_tap_s) z.tap_pending = false;
    }
  }

  uint8_t new_mask = 0;
  int down_count = 0;
  for (int i = 0; i < kZones; ++i) {
    if (z_[i].down) {
      new_mask |= static_cast<uint8_t>(1u << i);
      ++down_count;
    }
  }
  // Any pad going down marks every other pad currently held as part of
  // something multi-zone, so none of them will report a plain tap.
  for (int i = 0; i < kZones; ++i) {
    if (!went_down[i]) continue;
    for (int j = 0; j < kZones; ++j)
      if (j != i && z_[j].down) {
        z_[j].multi_seen = true;
        z_[i].multi_seen = true;
      }
  }
  swipe_last_s_ += dt_s;

  // Extend the swipe sequence on each new zone.
  for (int i = 0; i < kZones; ++i) {
    if (!went_down[i]) continue;
    if (swipe_len_ > 0 && swipe_last_s_ > cfg_.swipe_step_s) swipe_len_ = 0;
    if (swipe_len_ < kZones) swipe_seq_[swipe_len_++] = static_cast<int8_t>(i);
    swipe_last_s_ = 0.0f;
  }
  down_mask_ = new_mask;

  // A chord has to be held to count. Two zones arriving together starts a
  // candidate; it only becomes a chord if the same set is still down chord_s
  // later, which a finger sliding off the first pad never is.
  if (new_mask == 0) {
    chord_fired_ = false;
    chord_cand_ = 0;
    chord_cand_s_ = 0.0f;
  } else if (!chord_fired_) {
    if (down_count >= 2) {
      if (new_mask != chord_cand_) {
        chord_cand_ = new_mask;      // a third finger restarts the clock
        chord_cand_s_ = 0.0f;
      } else {
        chord_cand_s_ += dt_s;
        if (chord_cand_s_ >= cfg_.chord_s) {
          chord_fired_ = true;
          for (int i = 0; i < kZones; ++i)
            if (chord_cand_ & (1u << i)) z_[i].consumed = true;
          swipe_len_ = 0;  // a held chord is not the middle of a swipe
          n = emit(out, max, n, Event(EventType::Chord, chord_cand_));
        }
      }
    } else {
      chord_cand_ = 0;
      chord_cand_s_ = 0.0f;
    }
  }

  // A swipe is three zones in strict order where the first has already been
  // let go by the time the third arrives — the shape a dragged finger makes,
  // and the shape three pressed fingers do not.
  if (cfg_.enable_swipe && !chord_fired_ && swipe_len_ == kZones) {
    const int8_t a = swipe_seq_[0], b = swipe_seq_[1], c = swipe_seq_[2];
    const bool ordered = (a == 0 && b == 1 && c == 2) || (a == 2 && b == 1 && c == 0);
    if (ordered && !z_[a].down) {
      for (int i = 0; i < kZones; ++i) z_[i].consumed = true;
      n = emit(out, max, n, Event(EventType::Swipe, 0, a == 0 ? 1 : -1));
      swipe_len_ = 0;
    } else if (!ordered) {
      swipe_len_ = 0;
    }
  }

  for (int i = 0; i < kZones; ++i) {
    ZoneState& z = z_[i];
    const uint8_t w = static_cast<uint8_t>(i);

    if (z.down && !z.hold_fired && z.down_s >= cfg_.hold_s && !z.consumed) {
      z.hold_fired = true;
      z.tap_pending = false;  // a hold is not a tap and cannot be doubled
      n = emit(out, max, n, Event(EventType::HoldBegin, w));
    }

    if (!went_up[i]) continue;
    z.since_release_s = 0.0f;

    if (z.hold_fired) {
      n = emit(out, max, n, Event(EventType::HoldEnd, w));
    } else if (z.consumed || z.multi_seen) {
      // Part of a chord or a swipe — either already reported, or ambiguous
      // enough that doing nothing beats doing the wrong thing.
    } else if (z.tap_pending) {
      // The second tap supersedes the first, which has already fired.
      z.tap_pending = false;
      n = emit(out, max, n, Event(EventType::DoubleTap, w));
    } else {
      z.tap_pending = true;
      n = emit(out, max, n, Event(EventType::Tap, w));
    }
  }

  // ---------------------------------------------------------------- encoder
  // No priming: the interrupt's counter starts at zero and so does this, so
  // the first detent of the session is a real one. Priming on the first frame
  // would swallow it, which on hardware reads as the encoder ignoring the very
  // first click after boot.
  const int32_t det = in.encoder_detents;
  const int32_t ddet = det - last_detents_;
  last_detents_ = det;

  // Rate over a sliding window, so a long sweep can be made to step faster
  // without a single slow detent ever being multiplied.
  turn_win_s_ += dt_s;
  turn_accum_ += static_cast<float>(ddet < 0 ? -ddet : ddet);
  float rate = 0.0f;
  if (turn_win_s_ >= cfg_.turn_window_s) {
    rate = turn_accum_ / turn_win_s_;
    turn_accum_ = 0.0f;
    turn_win_s_ = 0.0f;
    last_rate_ = rate;
  } else {
    rate = last_rate_;
  }

  if (ddet != 0) {
    if (sw_down_) {
      sw_turned_while_down_ = true;
      n = emit(out, max, n,
               Event(EventType::PressTurn, 0, static_cast<int16_t>(ddet), rate));
    } else {
      n = emit(out, max, n, Event(EventType::Turn, 0, static_cast<int16_t>(ddet), rate));
    }
  }

  // The switch, with the same shape as a zone.
  if (in.encoder_sw != sw_down_) {
    sw_down_ = in.encoder_sw;
    if (sw_down_) {
      sw_down_s_ = 0.0f;
      sw_hold_fired_ = false;
      sw_turned_while_down_ = false;
    } else {
      sw_since_release_s_ = 0.0f;
      if (sw_hold_fired_) {
        n = emit(out, max, n, Event(EventType::PressHoldEnd));
      } else if (sw_turned_while_down_) {
        // Press-and-turn is a modifier, not a press. Releasing it does nothing.
      } else if (sw_press_pending_) {
        sw_press_pending_ = false;
        n = emit(out, max, n, Event(EventType::DoublePress));
      } else {
        sw_press_pending_ = true;
        n = emit(out, max, n, Event(EventType::Press));
      }
    }
  }
  if (sw_down_) {
    sw_down_s_ += dt_s;
    if (!sw_hold_fired_ && !sw_turned_while_down_ && sw_down_s_ >= cfg_.hold_s) {
      sw_hold_fired_ = true;
      sw_press_pending_ = false;
      n = emit(out, max, n, Event(EventType::PressHoldBegin));
    }
  } else {
    sw_since_release_s_ += dt_s;
    if (sw_press_pending_ && sw_since_release_s_ > cfg_.double_tap_s) {
      sw_press_pending_ = false;
    }
  }

  // ---------------------------------------------------------------- motion
  if (shake_lockout_s_ > 0.0f) shake_lockout_s_ -= dt_s;
  if (tap_lockout_s_ > 0.0f) tap_lockout_s_ -= dt_s;

  if (cfg_.enable_motion && in.motion_valid) {
    const float mag =
        std::sqrt(in.ax * in.ax + in.ay * in.ay + in.az * in.az);
    if (!motion_primed_) {
      motion_primed_ = true;
      prev_mag_ = mag;
    }
    const float jolt = std::fabs(mag - prev_mag_);
    prev_mag_ = mag;

    if (jolt >= cfg_.shake_g && shake_lockout_s_ <= 0.0f) {
      shake_lockout_s_ = 0.6f;
      tap_lockout_s_ = 0.6f;  // a shake is not also a tap
      n = emit(out, max, n, Event(EventType::Shake));
    } else if (jolt >= cfg_.tap_g && tap_lockout_s_ <= 0.0f) {
      tap_lockout_s_ = 0.35f;
      n = emit(out, max, n, Event(EventType::CaseTap));
    }

    // Standing on its front edge, gravity lies along an in-plane axis. Laid
    // flat it moves onto the board normal. Which signed axis is which has to be
    // measured on a real board; the test is written against the magnitude of
    // the in-plane component so the sign convention can change without it.
    const float in_plane = std::sqrt(in.ax * in.ax + in.ay * in.ay);
    const bool flat_now = in_plane < cfg_.flat_g;
    if (flat_now == is_flat_) {
      flat_s_ = 0.0f;
    } else {
      flat_s_ += dt_s;
      if (flat_s_ >= cfg_.flat_dwell_s) {
        is_flat_ = flat_now;
        flat_s_ = 0.0f;
        n = emit(out, max, n,
                 Event(is_flat_ ? EventType::LaidFlat : EventType::PickedUp));
      }
    }
  }

  return n > max ? max : n;
}

}  // namespace ui
