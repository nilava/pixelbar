// The device side of ui::Ports: real pins, real flash, real time.
//
// This is the only place in the firmware that knows about ESP-IDF and about the
// state machine at the same time.
//
// Called `board` and not `hal` because ESP-IDF already has a component by that
// name — the one that provides hal/gpio_types.h and hal/sha_types.h. A second
// component called hal shadows it, and the first thing you learn is that
// mbedtls cannot find its own headers. Everything above it — the recogniser, the
// model, the drawing — builds with plain g++ and is tested on a laptop; this
// file is what those things are plugged into when they meet a board.
#pragma once
#include "esp_err.h"
#include "ui/gesture.h"
#include "ui/ports.h"
#include "ui/virtual_pads.h"

namespace board {

// Sets up the encoder interrupt and NVS. Safe to call once, from app_main.
esp_err_t init();

class DevicePorts : public ui::Ports {
 public:
  void read_raw(ui::RawInput* out) override;
  bool load_settings(ui::Settings* out) override;
  bool save_settings(const ui::Settings& s) override;
  bool wall_clock(int* h, int* m, int* s) override;
  bool wifi_connected() override;

  // Called once a frame, before read_raw, to advance anything scheduled.
  void advance(float dt_s);

  // Pads driven from somewhere other than a finger. The three TTP223 modules
  // are not soldered on yet, so for now this is how the touch layer is reached
  // — from the web page, through exactly the same pin levels a pad would
  // produce. See ui::VirtualPads for why it is levels and not events.
  ui::VirtualPads& pads() { return pads_; }

  // Diagnostics worth having on the first board: a non-zero illegal count at
  // ordinary turning speed means the interrupt is being starved.
  int32_t encoder_detents() const;
  uint32_t encoder_illegal() const;

  void set_wifi(bool up) { wifi_ = up; }

 private:
  ui::VirtualPads pads_;
  bool wifi_ = false;
  double uptime_s_ = 0.0;
};

}  // namespace board
