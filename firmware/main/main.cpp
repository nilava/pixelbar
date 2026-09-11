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
#include "net/net.h"
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
  // Seeds the per-channel dither offsets. Without this every channel starts
  // level and they all cross the threshold on the same frame, which is a whole
  // panel shimmering in step — far more visible than the stepping the dither
  // exists to remove.
  renderer.reset_dither();
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

  // Pins, pull-ups, the encoder interrupt and NVS. Without this the inputs are
  // unconfigured: every pin floats, three of them wander in lockstep picking up
  // the same ambient noise, and the encoder interrupt does not exist — which
  // reads exactly like an encoder that is wired wrong.
  const esp_err_t hw = board::init();
  if (hw != ESP_OK) ESP_LOGE(TAG, "board init failed: %s", esp_err_to_name(hw));

  static board::DevicePorts ports;
  static ui::App app;
  app.begin(ports, 0.0);
  ESP_LOGI(TAG, "ready: turn the knob to change view, hold it for the menu");

  // WiFi and the web page. Started, not waited for: a device whose panel does
  // not light until a router answers is a worse device.
  const esp_err_t nerr = net_start();
  if (nerr != ESP_OK) ESP_LOGW(TAG, "network not available: %s", esp_err_to_name(nerr));

  // Which pins actually move when you turn the knob. Resting levels cannot
  // tell a floating pin from a correctly pulled-up one; edges can.
  constexpr bool kPinScan = false;
  if (kPinScan) board::scan_begin();

  const int64_t boot_us = esp_timer_get_time();
  bool mapping = kMapTestSeconds > 0;
  bool warned_about_power = false;
  ui::EventType last_ev = ui::EventType::None;
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
      if (kPinScan) board::scan_poll();

      // Drain whatever the web page asked for, once, at the top of the frame.
      //
      // Handlers run on the server's task and never touch the model: they put a
      // command on a queue and this is the only place it is read. Reaching into
      // the App from a handler would mutate state halfway through a frame that
      // is already being drawn from it, and the failure would be a rare torn
      // frame rather than anything a test would catch.
      net_cmd_t cmd;
      while (net_take_cmd(&cmd)) {
        switch (cmd.kind) {
          // Pads and knob arrive as levels and detents, so the recogniser sees
          // exactly what the hardware would produce.
          case NET_CMD_TAP: ports.pads().tap(cmd.a); break;
          case NET_CMD_HOLD: ports.pads().set_held(cmd.a, true); break;
          case NET_CMD_RELEASE: ports.pads().set_held(cmd.a, false); break;
          case NET_CMD_SWIPE: ports.pads().swipe(cmd.a); break;
          case NET_CMD_TURN: ports.nudge_encoder(cmd.a); break;
          case NET_CMD_PRESS: ports.press_switch(0.06f); break;
          case NET_CMD_PRESS_HOLD: ports.press_switch(0.60f); break;
          // These two are values rather than gestures, so they go straight in.
          case NET_CMD_STATUS: app.set_status_external(cmd.a); break;
          case NET_CMD_BRIGHTNESS: app.set_brightness_external(cmd.a); break;
          default: break;
        }
      }

      ports.advance(a.dt);
      app.update(a.dt, a.t);
      app.render(fb, a);

      // A live trace of what the recogniser made of the inputs. A press lasts
      // sixty milliseconds and a periodic log samples every fifteen hundred, so
      // without this the one thing you want to see is the one thing you never
      // catch. Logged on change rather than every frame.
      const ui::EventType ev = app.last_event();
      if (ev != last_ev) {
        last_ev = ev;
        if (ev != ui::EventType::None) {
          ESP_LOGI(TAG, "  %-14s -> %s / %s", ui::event_name(ev),
                   panel::screen_name(app.screen()),
                   panel::status_label(app.state().status));
        }
      }
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
    if (esp_timer_get_time() - last_log_us > 1500000) {
      last_log_us = esp_timer_get_time();
      // The encoder's illegal-transition count is here on purpose: a non-zero
      // value at ordinary turning speed means the interrupt is being starved,
      // and that is far easier to see in a log line than on the panel.
      ESP_LOGI(TAG,
               "fps=%.1f avg=%.2fms max=%.2fms power=%.0f%% screen=%s "
               "detents=%ld illegal=%lu  pads=%c%c%c enc A=%d B=%d sw=%d %s "
               "bright=%d(%d%%)",
               meter.fps(), meter.frame_ms_avg(), meter.frame_ms_max(),
               st.power_scale * 100.0f, panel::screen_name(app.screen()),
               static_cast<long>(ports.encoder_detents()),
               static_cast<unsigned long>(ports.encoder_illegal()),
               (ports.raw_pads() & 1) ? 'L' : '-',
               (ports.raw_pads() & 2) ? 'M' : '-',
               (ports.raw_pads() & 4) ? 'R' : '-',
               (ports.raw_encoder() & 1) ? 1 : 0,
               (ports.raw_encoder() & 2) ? 1 : 0,
               (ports.raw_encoder() & 4) ? 1 : 0,
               panel::status_label(app.state().status), app.state().brightness,
               (app.state().brightness * 100 + 127) / 255);
      if (kPinScan) {
        char scan[192];
        board::scan_report(scan, sizeof(scan));
        ESP_LOGI(TAG, "pins gpio:level/edges  %s", scan);
      }
      meter.reset_peak();
    }

    // What the web page shows. Written once a frame and read by the server's
    // task; a torn read would at worst mix two frames of a status display.
    {
      net_status_t ns = {};
      snprintf(ns.screen, sizeof(ns.screen), "%s", panel::screen_name(app.screen()));
      snprintf(ns.status, sizeof(ns.status), "%s",
               panel::status_label(app.state().status));
      ns.brightness = app.state().brightness;
      ns.timer_left_s = app.state().timer_left_s;
      ns.timer_running = app.state().timer_running;
      ns.fps = meter.fps();
      ns.detents = ports.encoder_detents();
      ns.illegal = ports.encoder_illegal();
      net_publish(&ns);
    }

    vTaskDelayUntil(&last_wake, period);
  }
}
