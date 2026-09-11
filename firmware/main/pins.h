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

}  // namespace pins
