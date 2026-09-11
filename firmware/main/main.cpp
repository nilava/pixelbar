// Pixelbar: 8x24 WS2812B desk panel on an ESP32-C3 SuperMini.
//
// The loop is three lines of work and a lot of care about when they happen:
// read the inputs and advance the model, draw it, clock it out. Everything
// interesting lives above this file, in two components that know nothing about
// ESP-IDF and are tested on a laptop.
#include <cstdio>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "panel/anim.h"
#include "panel/config.h"
#include "panel/framebuffer.h"
#include "board/board.h"
#include "panel/patterns.h"
#include "panel/renderer.h"
#include "panel/screens.h"
#include "panel/transition.h"
#include "pins.h"
#include "ui/app.h"
#include "ws2812.h"

namespace {

const char* TAG = "pixelbar";

// Hold the mapping test at boot, long enough to watch the head sweep the whole
// panel twice. Zero because kWiring is confirmed: it was taken from a WLED
// setup running on this panel, which is the only evidence that settles it.
// Pattern::MapTest is still there for whenever the wiring is in doubt again.
constexpr int64_t kMapTestSeconds = 0;

panel::micros_t now_us() {
  return static_cast<panel::micros_t>(esp_timer_get_time());
}

}  // namespace

extern "C" void app_main(void) {
  ESP_LOGI(TAG, "pixelbar: %d x %d, %d LEDs, %d fps", panel::kWidth, panel::kHeight,
           panel::kNumLeds, panel::kFramesPerSecond);

  ws2812_handle_t strip = nullptr;
  ESP_ERROR_CHECK(ws2812_new(pins::kLedData, panel::kNumLeds, &strip));
  ESP_ERROR_CHECK(ws2812_clear(strip));

  static panel::Framebuffer fb;
  static panel::Engine engine;
  static panel::ScreenManager screens;
  static panel::Renderer renderer;
  static panel::FrameClock clock;
  static panel::FpsMeter meter;
  // Double buffered: the RMT peripheral reads one while the next is drawn.
  static uint8_t wire[2][panel::kNumLeds * 3];
  int cur = 0;

  engine.set_pattern(panel::Pattern::MapTest);
  if (kMapTestSeconds > 0) {
    ESP_LOGI(TAG, "mapping test for %llds: the white pixel should sweep left to "
                  "right along the top row, starting at the red marker",
             static_cast<long long>(kMapTestSeconds));
  }

  static board::DevicePorts ports;
  static ui::App app;
  app.begin(ports, 0.0);
  ESP_LOGI(TAG, "ready: turn the knob to change view, hold it for the menu");

  const int64_t boot_us = esp_timer_get_time();
  bool mapping = kMapTestSeconds > 0;
  bool warned_about_power = false;
  int64_t last_log_us = boot_us;

  const TickType_t period = pdMS_TO_TICKS(1000 / panel::kFramesPerSecond);
  TickType_t last_wake = xTaskGetTickCount();

  while (true) {
    const panel::micros_t t_us = now_us();
    const int64_t uptime_s = (esp_timer_get_time() - boot_us) / 1000000;
    const panel::Anim a = clock.tick(t_us);

    if (mapping && uptime_s >= kMapTestSeconds) {
      mapping = false;
      ESP_LOGI(TAG, "mapping test done");
    }

    if (mapping) {
      // Wiring check: a crisp single pixel, so dithering is off for it.
      engine.render_us(fb, t_us);
    } else {
      ports.advance(a.dt);
      app.update(a.dt, a.t);
      app.render(fb, a);
    }

    const panel::RenderStats st =
        renderer.render(fb, wire[cur], app.state().brightness, app.max_ma(),
                        panel::kWiring);
    if (st.power_scale < 1.0f && !warned_about_power) {
      warned_about_power = true;
      ESP_LOGW(TAG, "power cap active: scaling to %.0f%% to stay under %.0f mA",
               st.power_scale * 100.0f, app.max_ma());
    }

    // Wait for the previous frame only now, after all the drawing: its time on
    // the wire has overlapped with this frame's work.
    const esp_err_t err = ws2812_wait(strip, 50);
    if (err != ESP_OK) ESP_LOGE(TAG, "led wait failed: %s", esp_err_to_name(err));
    ws2812_write_async(strip, wire[cur]);
    cur ^= 1;

    meter.tick(t_us);
    if (esp_timer_get_time() - last_log_us > 5000000) {
      last_log_us = esp_timer_get_time();
      // The encoder's illegal-transition count is here on purpose: a non-zero
      // value at ordinary turning speed means the interrupt is being starved,
      // and that is far easier to see in a log line than on the panel.
      ESP_LOGI(TAG,
               "fps=%.1f frame avg=%.2f ms max=%.2f ms  power=%.0f%%  "
               "screen=%s  detents=%ld illegal=%lu",
               meter.fps(), meter.frame_ms_avg(), meter.frame_ms_max(),
               st.power_scale * 100.0f, panel::screen_name(app.screen()),
               static_cast<long>(ports.encoder_detents()),
               static_cast<unsigned long>(ports.encoder_illegal()));
      meter.reset_peak();
    }

    vTaskDelayUntil(&last_wake, period);
  }
}
