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

constexpr int kFramesPerSecond = 60;

}  // namespace panel
