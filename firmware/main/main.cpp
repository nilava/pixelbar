// Pixelbar: 8x24 WS2812B desk panel on an ESP32-C3 SuperMini.
//
// Step 1 of the firmware: drive the panel. It boots into the mapping test so
// the wiring can be confirmed against panel::kWiring, then cycles the pattern
// set. Inputs, WiFi and the status state machine come next.
#include <cstdio>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "panel/config.h"
#include "panel/framebuffer.h"
#include "panel/patterns.h"
#include "pins.h"
#include "ws2812.h"

namespace {

const char* TAG = "pixelbar";

// How long to hold the mapping test after boot. Long enough to watch the head
// sweep the whole panel twice; set to 0 once the wiring is confirmed.
constexpr int64_t kMapTestSeconds = 24;
// Each show pattern gets this long before the next one.
constexpr int64_t kPatternSeconds = 10;

const panel::Pattern kShowPatterns[] = {
    panel::Pattern::Text,    panel::Pattern::Clock,  panel::Pattern::Rainbow,
    panel::Pattern::Plasma,  panel::Pattern::Sparkle,
};
constexpr int kShowCount = sizeof(kShowPatterns) / sizeof(kShowPatterns[0]);

int64_t now_us() { return esp_timer_get_time(); }

}  // namespace

extern "C" void app_main(void) {
  ESP_LOGI(TAG, "pixelbar: %d x %d, %d LEDs", panel::kWidth, panel::kHeight,
           panel::kNumLeds);

  ws2812_handle_t strip = nullptr;
  ESP_ERROR_CHECK(ws2812_new(pins::kLedData, panel::kNumLeds, &strip));
  ESP_ERROR_CHECK(ws2812_clear(strip));

  static panel::Framebuffer fb;
  static panel::Engine engine;
  static uint8_t wire[panel::kNumLeds * 3];

  engine.set_text("PIXELBAR");
  engine.set_pattern(panel::Pattern::MapTest);
  ESP_LOGI(TAG, "mapping test for %llds: the white pixel should sweep left to "
                "right along the top row, starting at the red marker",
           (long long)kMapTestSeconds);

  const int64_t boot_us = now_us();
  int64_t last_switch_us = boot_us;
  int show_index = -1;  // -1 while the mapping test is running
  bool warned_about_power = false;

  const TickType_t period = pdMS_TO_TICKS(1000 / panel::kFramesPerSecond);
  TickType_t last_wake = xTaskGetTickCount();

  while (true) {
    const int64_t t_us = now_us();
    const int64_t uptime_s = (t_us - boot_us) / 1000000;

    // Advance the demo rotation.
    if (show_index < 0) {
      if (uptime_s >= kMapTestSeconds) {
        show_index = 0;
        last_switch_us = t_us;
        engine.set_pattern(kShowPatterns[0]);
        ESP_LOGI(TAG, "mapping test done, cycling patterns");
      }
    } else if ((t_us - last_switch_us) / 1000000 >= kPatternSeconds) {
      show_index = (show_index + 1) % kShowCount;
      last_switch_us = t_us;
      engine.set_pattern(kShowPatterns[show_index]);
      ESP_LOGI(TAG, "pattern: %s", panel::pattern_name(engine.pattern()));
    }

    // Stand-in clock until NTP arrives in a later step: count from boot.
    const int64_t secs = uptime_s;
    engine.params.hour = static_cast<int>((secs / 3600) % 24);
    engine.params.minute = static_cast<int>((secs / 60) % 60);
    engine.params.second = static_cast<int>(secs % 60);

    engine.render(fb, static_cast<uint32_t>(t_us / 1000));

    const float scale = fb.render(wire, panel::kDefaultBrightness,
                                  panel::kMaxMilliamps, panel::kWiring);
    if (scale < 1.0f && !warned_about_power) {
      warned_about_power = true;
      ESP_LOGW(TAG, "power cap active: scaling to %.0f%% to stay under %.0f mA",
               scale * 100.0f, panel::kMaxMilliamps);
    }

    const esp_err_t err = ws2812_write(strip, wire);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "led write failed: %s", esp_err_to_name(err));
      vTaskDelay(pdMS_TO_TICKS(200));
    }

    vTaskDelayUntil(&last_wake, period);
  }
}
