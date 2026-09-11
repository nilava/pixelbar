// Pixelbar: 8x24 WS2812B desk panel on an ESP32-C3 SuperMini.
//
// The display layer is complete: screens, icons, transitions and continuous
// animation at 100 fps. Inputs are not wired yet, so until they are this runs
// a scripted tour that visits every screen and every transition, which is how
// the motion gets judged on real LEDs behind the diffuser.
#include <cstdio>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "panel/anim.h"
#include "panel/config.h"
#include "panel/framebuffer.h"
#include "panel/patterns.h"
#include "panel/renderer.h"
#include "panel/screens.h"
#include "panel/transition.h"
#include "pins.h"
#include "ws2812.h"

namespace {

const char* TAG = "pixelbar";

// Hold the mapping test at boot, long enough to watch the head sweep the
// whole panel twice. Set to 0 once kWiring is confirmed against the hardware.
constexpr int64_t kMapTestSeconds = 24;

// One stop on the tour.
struct Stop {
  panel::Screen screen;
  panel::Status status;
  float seconds;
};

// Deliberately ordered so each move exercises a different transition: the view
// cycle slides, the adjust screens wipe, a status change dissolves, sleep fades.
const Stop kTour[] = {
    {panel::Screen::Booting, panel::Status::Free, 4.0f},
    {panel::Screen::Status, panel::Status::Free, 4.0f},
    {panel::Screen::Status, panel::Status::Busy, 4.0f},   // dissolve
    {panel::Screen::Status, panel::Status::Call, 5.0f},   // dissolve
    {panel::Screen::Status, panel::Status::Dnd, 4.0f},    // dissolve
    {panel::Screen::Clock, panel::Status::Dnd, 6.0f},     // slide left
    {panel::Screen::Timer, panel::Status::Dnd, 8.0f},     // slide left
    {panel::Screen::Brightness, panel::Status::Dnd, 5.0f},// wipe up
    {panel::Screen::ColorPick, panel::Status::Dnd, 6.0f}, // wipe up
    {panel::Screen::TimerSet, panel::Status::Dnd, 5.0f},  // wipe up
    {panel::Screen::Status, panel::Status::Free, 4.0f},   // wipe down
    {panel::Screen::Sleep, panel::Status::Free, 5.0f},    // fade
};
constexpr int kTourLen = sizeof(kTour) / sizeof(kTour[0]);

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
  ESP_LOGI(TAG, "mapping test for %llds: the white pixel should sweep left to "
                "right along the top row, starting at the red marker",
           static_cast<long long>(kMapTestSeconds));

  panel::UiState ui;
  ui.accent = panel::RGB(255, 138, 31);
  ui.timer_total_s = 25 * 60;
  ui.timer_left_s = 25 * 60;
  ui.timer_running = true;
  ui.timer_set_min = 25;
  ui.brightness = panel::kDefaultBrightness;
  screens.set_screen(kTour[0].screen);

  const int64_t boot_us = esp_timer_get_time();
  int stop = 0;
  float stop_elapsed = 0.0f;
  bool touring = false;
  bool warned_about_power = false;
  int64_t last_log_us = boot_us;

  const TickType_t period = pdMS_TO_TICKS(1000 / panel::kFramesPerSecond);
  TickType_t last_wake = xTaskGetTickCount();

  while (true) {
    const panel::micros_t t_us = now_us();
    const int64_t uptime_s = (esp_timer_get_time() - boot_us) / 1000000;
    const panel::Anim a = clock.tick(t_us);

    if (!touring && uptime_s >= kMapTestSeconds) {
      touring = true;
      ESP_LOGI(TAG, "mapping test done, starting the screen tour");
    }

    if (!touring) {
      // Wiring check first: a crisp single pixel, so dithering is off for it.
      engine.render_us(fb, t_us);
    } else {
      stop_elapsed += a.dt;
      if (stop_elapsed >= kTour[stop].seconds) {
        stop_elapsed = 0.0f;
        const int prev = stop;
        stop = (stop + 1) % kTourLen;
        ui.status = kTour[stop].status;
        if (kTour[stop].screen == kTour[prev].screen) {
          // Same screen, new status: dissolve in place.
          screens.restart_with(panel::TransitionKind::Dissolve, 0.3f);
        } else {
          screens.go_to(kTour[stop].screen);
        }
        ESP_LOGI(TAG, "%s / %s", panel::screen_name(screens.current()),
                 panel::status_label(ui.status));
      }
      // A clock and a timer that actually move, so the tour shows real motion.
      ui.hour = static_cast<int>((uptime_s / 3600) % 24);
      ui.minute = static_cast<int>((uptime_s / 60) % 60);
      ui.second = static_cast<int>(uptime_s % 60);
      ui.timer_left_s = 25 * 60 - static_cast<int>(uptime_s * 3) % (25 * 60);
      ui.hue = panel::wrap01(static_cast<float>(a.t) * 0.05f);
      screens.render(fb, ui, a);
    }

    const panel::RenderStats st = renderer.render(fb, wire[cur], panel::kDefaultBrightness,
                                                  panel::kMaxMilliamps, panel::kWiring);
    if (st.power_scale < 1.0f && !warned_about_power) {
      warned_about_power = true;
      ESP_LOGW(TAG, "power cap active: scaling to %.0f%% to stay under %.0f mA",
               st.power_scale * 100.0f, panel::kMaxMilliamps);
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
      ESP_LOGI(TAG, "fps=%.1f frame avg=%.2f ms max=%.2f ms  power=%.0f%%",
               meter.fps(), meter.frame_ms_avg(), meter.frame_ms_max(),
               st.power_scale * 100.0f);
      meter.reset_peak();
    }

    vTaskDelayUntil(&last_wake, period);
  }
}
