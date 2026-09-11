// Build-time configuration for the panel itself.
#pragma once
#include "panel/geometry.h"

namespace panel {

// How the three 8x8 boards are wired.
//
// TODO: confirm on the real hardware. Flash, run Pattern::MapTest, and watch
// the white pixel. It should sweep left to right along the top row (the red
// marker is logical 0,0), then drop to the next row. If it zig-zags, snakes
// vertically or starts in the wrong corner, change the fields below until it
// sweeps cleanly. Most 65 mm 8x8 boards are TopLeft / Row / serpentine.
constexpr Wiring kWiring = {
    /*first=*/Corner::TopLeft,
    /*major=*/Axis::Row,
    /*serpentine=*/true,
    /*chainRightToLeft=*/false,
};

// Supply budget. The panel is fed from a 5 V USB-C source; hold well clear of
// what the cable and the connector are rated for, and leave room for the ESP32
// and the sensors. 2500 mA of LED draw on a 3 A supply is a safe ceiling.
constexpr float kMaxMilliamps = 2500.0f;

// Default user brightness, 0..255. The power cap is the real safety net; this
// is just where the panel starts.
constexpr uint8_t kDefaultBrightness = 48;

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
