// What the state machine needs from the world outside it.
//
// This is the seam that keeps the input and drawing layers buildable with
// plain g++. Everything that needs a peripheral — GPIO levels, an I2C sensor,
// NVS, a wall clock — is behind these calls, implemented against ESP-IDF on the
// device and against memory in the simulator. Nothing below this line includes
// an IDF header, which is what `test/run.sh` exists to prove.
//
// Every method has a default that does nothing useful but is never wrong, so a
// port can implement only the parts that exist yet.
#pragma once
#include "ui/event.h"
#include "ui/settings.h"

namespace ui {

struct Ports {
  virtual ~Ports() = default;

  // The raw pin and sensor state for this frame.
  virtual void read_raw(RawInput* out) { *out = RawInput{}; }

  // Returns false when nothing has been stored yet, leaving the defaults.
  virtual bool load_settings(Settings* out) { return false; }
  virtual bool save_settings(const Settings& s) { return false; }

  // Local wall clock. Returns false until there is a real time source, which
  // is what makes the Clock screen show `--:--` rather than a plausible lie.
  virtual bool wall_clock(int* h, int* m, int* s) { return false; }

  virtual bool wifi_connected() { return false; }

  // What the device is doing about the network. Declared here rather than
  // taken from net.h because nothing in `ui` may include an ESP-IDF header —
  // the values are the same three states, and the device side maps between
  // them.
  enum class NetMode : uint8_t { Setup, Joining, Online };
  virtual NetMode net_mode() { return NetMode::Online; }
  // The setup SSID to join, or the address to visit, depending on the mode.
  // Must outlive the frame; on the device it is a static buffer in `net`.
  virtual const char* net_text() { return ""; }
};

}  // namespace ui
