// WS2812B output over SPI2 with GDMA.
//
// The RMT peripheral was the obvious choice and it is the wrong one on this
// chip. SOC_RMT_SUPPORT_DMA is defined for the ESP32-S3 and absent for the C3,
// so RMT here is refilled from an interrupt: a 4,608-symbol frame arrives 48
// symbols at a time, 96 refills a frame, 9,600 a second, each with about 58 us
// of runway. A WS2812B latches after roughly 50 us of idle line, so a single
// late refill makes the strip latch a partial frame and restart at LED 0. On
// the bench that showed up as a flicker travelling along the panel, worse while
// the encoder was being turned, because the GPIO interrupt that decodes it was
// competing for the same microseconds.
//
// Widening the buffer and raising the interrupt priority made the race easier
// to win. It did not stop it being a race. WiFi will be far less polite than an
// encoder, so the deadline has to stop existing rather than get easier.
//
// SPI can. Each WS2812 bit becomes four SPI bits at 3.2 MHz — 312.5 ns each, so
// one LED bit is exactly 1.25 us — and GDMA clocks the whole frame out of SRAM
// with no CPU and no interrupt latency anywhere in it.
//
//   0 -> 1000   312 ns high, 937 ns low   (T0H spec 220-380 ns)
//   1 -> 1110   937 ns high, 312 ns low   (T1H spec 580-1000 ns)
//
// The reset gap is just more zero bytes on the end: at 312.5 ns a bit, 112
// bytes hold the line low for 280 us.
#include "ws2812.h"

#if WS2812_BACKEND_SPI

#include <stdlib.h>
#include <string.h>

#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_log.h"

static const char* TAG = "ws2812";

#define WS2812_SPI_HZ 3200000
#define WS2812_SPI_BITS_PER_LED_BIT 4
// 280 us of idle line, in whole bytes at 312.5 ns per bit.
#define WS2812_RESET_BYTES 112

struct ws2812_strip {
  spi_device_handle_t dev;
  int led_count;
  uint8_t* dma;      // expanded bits, plus the reset gap
  size_t dma_len;
  uint8_t* scratch;  // for ws2812_clear
  spi_transaction_t txn;
  bool busy;
};

// One nibble of source becomes two bytes of SPI. Four entries of 1000/1110 per
// nibble, most significant source bit first.
static const uint16_t kNibble[16] = {
    0x8888, 0x888E, 0x88E8, 0x88EE, 0x8E88, 0x8E8E, 0x8EE8, 0x8EEE,
    0xE888, 0xE88E, 0xE8E8, 0xE8EE, 0xEE88, 0xEE8E, 0xEEE8, 0xEEEE,
};

// grb -> SPI bits. Runs on the caller's thread before the transfer starts, so
// by the time DMA is reading, this buffer is final and the caller's is free.
static void expand(const uint8_t* grb, size_t bytes, uint8_t* out) {
  for (size_t i = 0; i < bytes; ++i) {
    const uint8_t b = grb[i];
    const uint16_t hi = kNibble[b >> 4];
    const uint16_t lo = kNibble[b & 0x0F];
    *out++ = (uint8_t)(hi >> 8);
    *out++ = (uint8_t)(hi & 0xFF);
    *out++ = (uint8_t)(lo >> 8);
    *out++ = (uint8_t)(lo & 0xFF);
  }
}

esp_err_t ws2812_new(gpio_num_t gpio, int led_count, ws2812_handle_t* out) {
  ESP_RETURN_ON_FALSE(out && led_count > 0, ESP_ERR_INVALID_ARG, TAG, "bad args");

  struct ws2812_strip* s = calloc(1, sizeof(struct ws2812_strip));
  if (!s) return ESP_ERR_NO_MEM;
  s->led_count = led_count;

  const size_t payload = (size_t)led_count * 3;
  s->dma_len = payload * WS2812_SPI_BITS_PER_LED_BIT + WS2812_RESET_BYTES;
  // DMA-capable and zeroed: the reset tail is zeros and never rewritten.
  s->dma = heap_caps_calloc(1, s->dma_len, MALLOC_CAP_DMA);
  s->scratch = calloc(payload, 1);
  if (!s->dma || !s->scratch) {
    free(s->dma);
    free(s->scratch);
    free(s);
    return ESP_ERR_NO_MEM;
  }

  const spi_bus_config_t bus = {
      .mosi_io_num = gpio,
      .miso_io_num = -1,
      .sclk_io_num = -1,
      .quadwp_io_num = -1,
      .quadhd_io_num = -1,
      .max_transfer_sz = (int)s->dma_len,
  };
  esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
  if (err != ESP_OK) goto fail;

  const spi_device_interface_config_t dev = {
      .clock_speed_hz = WS2812_SPI_HZ,
      .mode = 0,
      .spics_io_num = -1,
      .queue_size = 1,
      // No chip select and no dummy bits: the data line is the whole protocol.
      .flags = SPI_DEVICE_NO_DUMMY,
  };
  err = spi_bus_add_device(SPI2_HOST, &dev, &s->dev);
  if (err != ESP_OK) {
    spi_bus_free(SPI2_HOST);
    goto fail;
  }

  ESP_LOGI(TAG, "%d LEDs on GPIO%d, SPI2 + DMA, %d byte buffer", led_count,
           (int)gpio, (int)s->dma_len);
  *out = s;
  return ESP_OK;

fail:
  free(s->dma);
  free(s->scratch);
  free(s);
  return err;
}

esp_err_t ws2812_write_async(ws2812_handle_t h, const uint8_t* grb) {
  ESP_RETURN_ON_FALSE(h && grb, ESP_ERR_INVALID_ARG, TAG, "bad args");
  if (h->busy) ESP_RETURN_ON_ERROR(ws2812_wait(h, 100), TAG, "previous frame");

  expand(grb, (size_t)h->led_count * 3, h->dma);

  memset(&h->txn, 0, sizeof(h->txn));
  h->txn.length = h->dma_len * 8;
  h->txn.tx_buffer = h->dma;
  ESP_RETURN_ON_ERROR(spi_device_queue_trans(h->dev, &h->txn, 0), TAG, "queue");
  h->busy = true;
  return ESP_OK;
}

esp_err_t ws2812_wait(ws2812_handle_t h, int timeout_ms) {
  ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "bad args");
  if (!h->busy) return ESP_OK;
  spi_transaction_t* done = NULL;
  const esp_err_t err = spi_device_get_trans_result(
      h->dev, &done, timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms));
  if (err == ESP_OK) h->busy = false;
  return err;
}

bool ws2812_busy(ws2812_handle_t h) { return h && h->busy; }

esp_err_t ws2812_write(ws2812_handle_t h, const uint8_t* grb) {
  ESP_RETURN_ON_ERROR(ws2812_write_async(h, grb), TAG, "queue failed");
  return ws2812_wait(h, 100);
}

esp_err_t ws2812_clear(ws2812_handle_t h) {
  ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "bad args");
  memset(h->scratch, 0, (size_t)h->led_count * 3);
  return ws2812_write(h, h->scratch);
}

void ws2812_del(ws2812_handle_t h) {
  if (!h) return;
  ws2812_wait(h, 100);
  spi_bus_remove_device(h->dev);
  spi_bus_free(SPI2_HOST);
  free(h->dma);
  free(h->scratch);
  free(h);
}

#endif  // WS2812_BACKEND_SPI
