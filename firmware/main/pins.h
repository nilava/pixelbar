// GPIO map for the ESP32-C3 SuperMini, matching hardware/wiring.
//
// GPIO2, 8 and 9 are strapping pins on the C3 and are deliberately left
// unused. GPIO20 and 21 are the UART0 pins, free to use because the console
// runs over USB Serial/JTAG; keep "USB CDC on boot" enabled or the encoder
// will fight the log output.
#pragma once
#include "hal/gpio_types.h"

namespace pins {

constexpr gpio_num_t kLedData = GPIO_NUM_10;  // to board 1 DIN, via 330 R

// Wired in step 2, listed here so the map lives in one place.
constexpr gpio_num_t kTouchLeft = GPIO_NUM_3;
constexpr gpio_num_t kTouchMiddle = GPIO_NUM_4;
constexpr gpio_num_t kTouchRight = GPIO_NUM_5;
constexpr gpio_num_t kI2cSda = GPIO_NUM_6;   // MPU-6050
constexpr gpio_num_t kI2cScl = GPIO_NUM_7;
constexpr gpio_num_t kMpuInt = GPIO_NUM_1;   // optional, wake on tap
constexpr gpio_num_t kEncoderA = GPIO_NUM_20;
constexpr gpio_num_t kEncoderB = GPIO_NUM_21;
constexpr gpio_num_t kEncoderSw = GPIO_NUM_0;

// Which inputs are physically present.
//
// An unpopulated pin is not a quiet pin. With nothing driving it, a weak
// internal pull is all that holds it, and a 192-LED data line switching a
// metre of unshielded wire away is enough to beat that: the first board read
// its unwired left and right pads as touched, which the state machine dutifully
// took for a chord and put the panel to sleep.
//
// So the firmware is told what exists rather than inferring it. A pin that is
// not fitted is never read, and the layer above it is absent rather than wrong
// — the same reason the accelerometer reports motion_valid false instead of a
// plausible stationary reading.
// Soldered. The modules drive their output actively and the pins are
// configured with pull-downs, so a module that comes loose reads untouched
// rather than floating — which is the failure this flag was added for.
//
// TTP223 boards ship momentary and active high, which is what the reader
// above assumes. Both are solder-jumper options on the module: bridging them
// for toggle mode would make every touch latch until the next one, and for
// active low would make the panel read as permanently held.
constexpr bool kTouchFitted = true;
constexpr bool kEncoderFitted = true;
constexpr bool kEncoderSwitchFitted = true;
constexpr bool kMotionFitted = false;        // MPU-6050 not fitted

}  // namespace pins
