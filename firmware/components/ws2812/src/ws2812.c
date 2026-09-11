#include "ws2812.h"

#if !WS2812_BACKEND_SPI

#include <stdlib.h>
#include <string.h>

#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_check.h"
#include "esp_log.h"

static const char* TAG = "ws2812";

// 10 MHz means one RMT tick is 0.1 us, which divides the WS2812B timings
// cleanly and keeps the symbol durations well inside the 15-bit field.
#define WS2812_RESOLUTION_HZ 10000000
#define T0H_TICKS 3   // 0.3 us
#define T0L_TICKS 9   // 0.9 us
#define T1H_TICKS 9   // 0.9 us
#define T1L_TICKS 3   // 0.3 us
#define RESET_TICKS 2800  // 280 us low, comfortably over the 50 us minimum

struct ws2812_strip {
  rmt_channel_handle_t channel;
  rmt_encoder_handle_t encoder;
  int led_count;
  uint8_t* buf;  // scratch for ws2812_clear
  bool busy;     // a frame is on the wire
};

// ---------------------------------------------------------------- encoder
//
// Two stages: a bytes encoder that turns each bit into one RMT symbol, then a
// copy encoder that appends the reset gap which latches the frame.

typedef struct {
  rmt_encoder_t base;
  rmt_encoder_t* bytes;
  rmt_encoder_t* copy;
  int state;
  rmt_symbol_word_t reset_code;
} led_encoder_t;

static size_t led_encode(rmt_encoder_t* encoder, rmt_channel_handle_t channel,
                         const void* data, size_t data_size,
                         rmt_encode_state_t* ret_state) {
  led_encoder_t* enc = __containerof(encoder, led_encoder_t, base);
  rmt_encode_state_t session = RMT_ENCODING_RESET;
  size_t encoded = 0;

  switch (enc->state) {
    case 0:  // pixel data
      encoded += enc->bytes->encode(enc->bytes, channel, data, data_size, &session);
      if (session & RMT_ENCODING_COMPLETE) {
        enc->state = 1;
      }
      if (session & RMT_ENCODING_MEM_FULL) {
        *ret_state = RMT_ENCODING_MEM_FULL;
        return encoded;
      }
      __attribute__((fallthrough));
    case 1:  // reset gap
      encoded += enc->copy->encode(enc->copy, channel, &enc->reset_code,
                                   sizeof(enc->reset_code), &session);
      if (session & RMT_ENCODING_COMPLETE) {
        enc->state = 0;
        *ret_state = RMT_ENCODING_COMPLETE;
        return encoded;
      }
      if (session & RMT_ENCODING_MEM_FULL) {
        *ret_state = RMT_ENCODING_MEM_FULL;
        return encoded;
      }
      break;
    default:
      break;
  }
  *ret_state = session;
  return encoded;
}

static esp_err_t led_encoder_reset(rmt_encoder_t* encoder) {
  led_encoder_t* enc = __containerof(encoder, led_encoder_t, base);
  rmt_encoder_reset(enc->bytes);
  rmt_encoder_reset(enc->copy);
  enc->state = 0;
  return ESP_OK;
}

static esp_err_t led_encoder_del(rmt_encoder_t* encoder) {
  led_encoder_t* enc = __containerof(encoder, led_encoder_t, base);
  rmt_del_encoder(enc->bytes);
  rmt_del_encoder(enc->copy);
  free(enc);
  return ESP_OK;
}

static esp_err_t make_encoder(rmt_encoder_handle_t* out) {
  led_encoder_t* enc = calloc(1, sizeof(led_encoder_t));
  if (!enc) return ESP_ERR_NO_MEM;

  enc->base.encode = led_encode;
  enc->base.reset = led_encoder_reset;
  enc->base.del = led_encoder_del;

  const rmt_bytes_encoder_config_t bytes_cfg = {
      .bit0 = {.level0 = 1, .duration0 = T0H_TICKS, .level1 = 0, .duration1 = T0L_TICKS},
      .bit1 = {.level0 = 1, .duration0 = T1H_TICKS, .level1 = 0, .duration1 = T1L_TICKS},
      .flags.msb_first = 1,  // WS2812B takes the most significant bit first
  };
  esp_err_t err = rmt_new_bytes_encoder(&bytes_cfg, &enc->bytes);
  if (err != ESP_OK) {
    free(enc);
    return err;
  }
  const rmt_copy_encoder_config_t copy_cfg = {};
  err = rmt_new_copy_encoder(&copy_cfg, &enc->copy);
  if (err != ESP_OK) {
    rmt_del_encoder(enc->bytes);
    free(enc);
    return err;
  }
  // A long low pulse; the level is what matters, split across the two halves.
  enc->reset_code = (rmt_symbol_word_t){
      .level0 = 0, .duration0 = RESET_TICKS / 2,
      .level1 = 0, .duration1 = RESET_TICKS / 2,
  };
  *out = &enc->base;
  return ESP_OK;
}

// ---------------------------------------------------------------- public API

esp_err_t ws2812_new(gpio_num_t gpio, int led_count, ws2812_handle_t* out) {
  ESP_RETURN_ON_FALSE(out && led_count > 0, ESP_ERR_INVALID_ARG, TAG, "bad args");

  struct ws2812_strip* s = calloc(1, sizeof(struct ws2812_strip));
  if (!s) return ESP_ERR_NO_MEM;
  s->led_count = led_count;
  s->buf = calloc((size_t)led_count * 3, 1);
  if (!s->buf) {
    free(s);
    return ESP_ERR_NO_MEM;
  }

  // Two things here are about deadlines rather than about LEDs.
  //
  // The ESP32-C3's RMT has no DMA — SOC_RMT_SUPPORT_DMA is defined for the S3
  // and absent here — so the peripheral is refilled from an interrupt. A frame
  // is 4,608 symbols; with a 48-symbol half-buffer that is 96 refills per
  // frame, 9,600 a second, each with about 58 us of runway. Miss one and the
  // strip sees an idle line, latches a partial frame and restarts at LED 0,
  // which shows up as a few LEDs lighting that should be dark.
  //
  // mem_block_symbols takes all four of the group's blocks. Only one TX channel
  // is ever used, so the other three are free, and claiming them doubles the
  // runway to about 115 us at no cost.
  //
  // intr_priority puts the refill above the GPIO interrupt that decodes the
  // encoder. Without it, turning the knob delays the refill: the first board
  // glitched visibly while the knob was moving and was clean when it was not.
  const rmt_tx_channel_config_t ch_cfg = {
      .gpio_num = gpio,
      .clk_src = RMT_CLK_SRC_DEFAULT,
      .resolution_hz = WS2812_RESOLUTION_HZ,
      .mem_block_symbols = 192,
      .trans_queue_depth = 4,
      .intr_priority = 3,
  };
  esp_err_t err = rmt_new_tx_channel(&ch_cfg, &s->channel);
  if (err != ESP_OK) goto fail;

  err = make_encoder(&s->encoder);
  if (err != ESP_OK) goto fail;

  err = rmt_enable(s->channel);
  if (err != ESP_OK) goto fail;

  ESP_LOGI(TAG, "%d LEDs on GPIO%d", led_count, (int)gpio);
  *out = s;
  return ESP_OK;

fail:
  if (s->encoder) rmt_del_encoder(s->encoder);
  if (s->channel) rmt_del_channel(s->channel);
  free(s->buf);
  free(s);
  return err;
}

esp_err_t ws2812_write_async(ws2812_handle_t h, const uint8_t* grb) {
  ESP_RETURN_ON_FALSE(h && grb, ESP_ERR_INVALID_ARG, TAG, "bad args");
  const rmt_transmit_config_t tx_cfg = {.loop_count = 0};
  ESP_RETURN_ON_ERROR(
      rmt_transmit(h->channel, h->encoder, grb, (size_t)h->led_count * 3, &tx_cfg),
      TAG, "transmit failed");
  h->busy = true;
  return ESP_OK;
}

esp_err_t ws2812_wait(ws2812_handle_t h, int timeout_ms) {
  ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "bad args");
  if (!h->busy) return ESP_OK;
  const esp_err_t err = rmt_tx_wait_all_done(h->channel, timeout_ms);
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
  memset(h->buf, 0, (size_t)h->led_count * 3);
  return ws2812_write(h, h->buf);
}

void ws2812_del(ws2812_handle_t h) {
  if (!h) return;
  ws2812_wait(h, 100);
  rmt_disable(h->channel);
  rmt_del_encoder(h->encoder);
  rmt_del_channel(h->channel);
  free(h->buf);
  free(h);
}

#endif  // !WS2812_BACKEND_SPI
