// Everything the device remembers across a power cut.
//
// One versioned struct rather than forty named values, because it is written
// as a single NVS blob: one atomic write, and a migration is a version check
// instead of forty defaulted lookups. The cost is that adding a field means
// bumping the version and teaching `migrate` about it, which is the right
// amount of friction for something that has to survive a firmware update.
#pragma once
#include <cstdint>

namespace ui {

struct Settings {
  static constexpr uint16_t kVersion = 1;
  uint16_t version = kVersion;

  // Display.
  uint8_t brightness = 48;
  float hue = 0.08f;             // the accent colour, as a hue
  uint16_t sleep_after_min = 0;  // 0 means never
  // Milliamps of LED draw the render path is allowed to ask for. Lower it for
  // a smaller bench supply; the cap scales frames that would exceed it.
  uint16_t max_ma = 2500;
  bool flip = false;             // the case turned the other way up

  // Focus timer.
  uint16_t work_min = 25;
  uint16_t rest_min = 5;
  uint8_t cycles = 4;
  bool auto_next = true;

  // Behaviour.
  uint8_t scene = 0;
  bool clock_24h = true;
  bool motion_enabled = true;
  bool flat_sleeps = true;
  bool touch_locked = false;

  // Clamps every field into range. Called after loading, so a corrupt or
  // truncated blob degrades to something usable rather than to a panel that
  // draws nothing at brightness zero.
  void sanitise();

  // Brings an older blob forward. Returns false if it is too old or too new to
  // make sense of, in which case the caller should keep the defaults.
  bool migrate();
};

}  // namespace ui
