// Build-time configuration for the panel itself.
#pragma once
#include "panel/geometry.h"

namespace panel {

// How the three 8x8 boards are wired.
//
// Confirmed against a working WLED setup on this exact panel, which is the
// only kind of evidence that settles this. WLED describes the same four
// things, under different names:
//
//   WLED                          here
//   1st LED: Top / Left           Corner::TopLeft
//   Orientation: Horizontal       Axis::Row
//   Serpentine: unchecked         serpentine = false
//   Panel offsets X = 0, 8, 16    chainRightToLeft = false
//
// The boards are *progressive*, not serpentine: every row runs left to right
// and the chain drops back to the left at the end of each one. This was the
// one genuinely unconfirmed value in the project, and the guess in it was
// wrong — "most 65 mm boards are serpentine" is true of many, and not of
// these.
constexpr Wiring kWiring = {
    /*first=*/Corner::TopLeft,
    /*major=*/Axis::Row,
    /*serpentine=*/false,
    /*chainRightToLeft=*/false,
};

// Supply budget, in milliamps of LED draw.
//
// A setting rather than a constant, because the panel has to be correct on
// both the supply it is developed on and the one it ships against. The bench
// prototype runs from a 2 A brick; the finished device is fed through the
// USB-C breakout from a 3 A source.
//
// Nothing in the drawing layer is tuned against a particular number. The heavy
// frames — a full-width status badge, the completion flash — are held under
// 2300 mA so that at the design target they never trip the cap, which keeps
// brightness consistent between screens. On the smaller supply the cap does
// engage, but only above about 85% brightness: below that, everything fits.
constexpr float kMaxMilliamps = 2500.0f;   // USB-C at 3 A, the design target
constexpr float kBenchMilliamps = 1900.0f; // the 2 A prototype supply
constexpr float kMinMilliamps = 400.0f;
constexpr float kMaxAllowedMilliamps = 3000.0f;

// Per-LED draw at full white, for the estimate in the render path.
//
// 60 mA is the datasheet figure, three channels at 20. WLED uses 55 as a
// measured typical for the same part, so this runs about 9% pessimistic —
// which is the direction to be wrong in for a current estimate.

// Default user brightness, 0..255. The power cap is the real safety net; this
// is just where the panel starts.
constexpr uint8_t kDefaultBrightness = 48;

// The dimmest the panel is allowed to go. See Settings::sanitise for the
// measurement behind it.
constexpr uint8_t kMinBrightness = 24;

// 100 fps, chosen because 1000 % 100 == 0: the 1 ms FreeRTOS tick paces it
// exactly, with no drift accumulator. The previous value of 60 was silently
// running at 62.5, because pdMS_TO_TICKS(1000/60) truncates to 16.
constexpr int kFramesPerSecond = 100;
constexpr int kFramePeriodUs = 1000000 / kFramesPerSecond;

static_assert(1000 % kFramesPerSecond == 0,
              "pick a frame rate that divides 1000 so the 1 ms tick paces it exactly");

// One frame on the wire: 192 LEDs x 24 bits x 1.2 us, plus the 280 us reset.
constexpr float kWireTimeMs = 5.81f;
static_assert(1000.0f / kFramesPerSecond > kWireTimeMs,
              "the frame period is shorter than the time it takes to clock a "
              "frame out; the panel cannot keep up");

}  // namespace panel
