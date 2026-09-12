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

// Watches every pin the encoder could plausibly be on and reports which ones
// actually move.
//
// A resting level tells you almost nothing — a floating pin and a correctly
// pulled-up one read the same. What separates them is whether the level ever
// changes when you turn the knob, and that is a question only the hardware can
// answer. Call scan_begin() once, then scan_report() to see the tally.
void scan_begin();
void scan_poll();
void scan_report(char* out, int cap);

class DevicePorts : public ui::Ports {
 public:
  void read_raw(ui::RawInput* out) override;
  bool load_settings(ui::Settings* out) override;
  bool save_settings(const ui::Settings& s) override;
  bool wall_clock(int* h, int* m, int* s) override;
  bool wifi_connected() override;
  ui::Ports::NetMode net_mode() override;
  const char* net_text() override;

  // Pushed in from main, for the same reason as the clock: this component
  // knows nothing about the radio.
  float ota_progress() override { return ota_; }
  void set_ota(float p) { ota_ = p; }
  uint32_t passkey() override { return passkey_; }
  int64_t unix_time() override;
  bool take_draw(ui::DrawPayload* out) override;
  void set_passkey(uint32_t k) { passkey_ = k; }

  int paired_count() override;
  const char* paired_name(int index) override;
  void begin_pairing() override;
  void forget_hosts() override;
  void forget_network() override;

  void set_net(ui::Ports::NetMode m, const char* text) {
    net_mode_ = m;
    net_text_ = text ? text : "";
  }

  // Called once a frame, before read_raw, to advance anything scheduled.
  void advance(float dt_s);

  // Pads driven from somewhere other than a finger. The three TTP223 modules
  // are not soldered on yet, so for now this is how the touch layer is reached
  // — from the web page, through exactly the same pin levels a pad would
  // produce. See ui::VirtualPads for why it is levels and not events.
  ui::VirtualPads& pads() { return pads_; }

  // The encoder and its switch, driven from somewhere other than a finger.
  // Same shape as the pads: a nudge adds detents to the same counter the
  // interrupt writes, and a press holds the switch level down for a while, so
  // the recogniser cannot tell the difference and neither can anything above it.
  void nudge_encoder(int detents);
  void press_switch(float seconds);

  // Diagnostics worth having on the first board: a non-zero illegal count at
  // ordinary turning speed means the interrupt is being starved.
  int32_t encoder_detents() const;
  uint32_t encoder_illegal() const;

  // The pins exactly as they read, before debouncing, virtual pads or any
  // interpretation at all. When the panel does something you did not ask for,
  // this is the difference between knowing which wire is wrong and guessing.
  uint8_t raw_pads() const;     // bit 0 left, 1 middle, 2 right
  uint8_t raw_encoder() const;  // bit 0 A, 1 B, 2 switch

  void set_wifi(bool up) { wifi_ = up; }
  // Pushed in from main rather than read from `net` directly, so this
  // component keeps knowing nothing about the radio. Until it is true
  // there is no time and wall_clock() says so.
  void set_time_valid(bool ok) { time_valid_ = ok; }

 private:
  ui::VirtualPads pads_;
  int32_t virtual_detents_ = 0;
  float virtual_press_s_ = 0.0f;
  bool wifi_ = false;
  bool time_valid_ = false;
  ui::Ports::NetMode net_mode_ = ui::Ports::NetMode::Online;
  const char* net_text_ = "";
  float ota_ = -1.0f;
  uint32_t passkey_ = 0;
  double uptime_s_ = 0.0;
};

}  // namespace board
