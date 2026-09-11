// WS2812B output over the ESP32-C3 RMT peripheral.
//
// Deliberately dependency-free: it uses only the RMT driver that ships with
// ESP-IDF, so the project builds offline with no managed components to fetch.
#pragma once
#include <stdint.h>

#include "esp_err.h"
#include "hal/gpio_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ws2812_strip* ws2812_handle_t;

// led_count is the number of LEDs in the chain.
esp_err_t ws2812_new(gpio_num_t gpio, int led_count, ws2812_handle_t* out);

// grb points at led_count*3 bytes in wire order: green, red, blue per LED.
// Blocks until the frame has been clocked out, then holds the reset gap.
esp_err_t ws2812_write(ws2812_handle_t h, const uint8_t* grb);

// Writes an all-zero frame.
esp_err_t ws2812_clear(ws2812_handle_t h);

void ws2812_del(ws2812_handle_t h);

#ifdef __cplusplus
}
#endif
