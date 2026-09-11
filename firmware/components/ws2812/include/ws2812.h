// WS2812B output over the ESP32-C3 RMT peripheral.
//
// Deliberately dependency-free: it uses only the RMT driver that ships with
// ESP-IDF, so the project builds offline with no managed components to fetch.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Which peripheral clocks the data line out.
//
// SPI, because the ESP32-C3's RMT has no DMA and therefore cannot meet a 50 us
// latch deadline reliably once anything else wants an interrupt. The RMT
// backend is kept for comparison on the bench — set this to 0 to build it —
// but it is not the one to ship on this chip. See ws2812_spi.c.
#ifndef WS2812_BACKEND_SPI
#define WS2812_BACKEND_SPI 1
#endif

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

// Queues a frame and returns at once.
//
// The RMT encoder reads grb incrementally from the interrupt as the frame
// clocks out, so the buffer must stay valid and unmodified until the matching
// ws2812_wait returns. Callers double-buffer. This is what lets the next frame
// be drawn during the 5.8 ms the current one spends on the wire.
esp_err_t ws2812_write_async(ws2812_handle_t h, const uint8_t* grb);

// Blocks until nothing is in flight. Returns immediately if already idle.
esp_err_t ws2812_wait(ws2812_handle_t h, int timeout_ms);

bool ws2812_busy(ws2812_handle_t h);

// Writes an all-zero frame.
esp_err_t ws2812_clear(ws2812_handle_t h);

void ws2812_del(ws2812_handle_t h);

#ifdef __cplusplus
}
#endif
