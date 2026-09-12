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

// What a host can ask the panel to show. A flat struct with no allocation and
// no pointers out, so it can be copied across the seam by value.
struct DrawPayload {
  char text[48] = {0};
  char icon[12] = {0};      // a name, looked up by panel::icon_by_name
  char source[16] = {0};    // who asked; equal priority from a different
                            // source replaces rather than stacking
  uint8_t priority = 50;    // 1..100; see kDrawPriority* in app.h
  float ttl_s = 10.0f;      // 0 means "until something replaces it"
  int64_t until_unix = 0;   // countdown target, 0 for none
  float bar = -1.0f;        // 0..1 draws a rail, negative draws none
  uint32_t tint = 0xFF8A1F;
};


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

  // Seconds since the epoch, or 0 when there is no time source.
  //
  // wall_clock gives the hours and minutes a clock face needs; this gives the
  // absolute moment a countdown needs. A host sends a deadline rather than a
  // duration, because a duration is stale by however long the message took to
  // arrive, and a deadline is not.
  virtual int64_t unix_time() { return 0; }

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

  // How far through a firmware update, 0..1, or negative when none is running.
  // The panel takes this over everything else while it is not negative: an
  // update is the one state where what the device is doing matters more than
  // whatever you were looking at.
  virtual float ota_progress() { return -1.0f; }

  // The Bluetooth pairing code, or 0 when nothing is pairing. Like an update,
  // this takes the panel: it is the one moment where what the device needs to
  // say matters more than whatever you were looking at.
  virtual uint32_t passkey() { return 0; }

  // Who this panel is paired with.
  //
  // Names are borrowed, not copied: on the device they point into the auth
  // store, which changes only when something pairs or is forgotten, so they
  // outlive any frame that draws them. The model re-reads the list when it
  // opens the screen rather than holding it, for the same reason.
  virtual int paired_count() { return 0; }
  virtual const char* paired_name(int index) { return ""; }

  // What the settings tree can *do* rather than set.
  //
  // All three are one-way: there is no value to read back, which is why they
  // are a kind of their own rather than a toggle nobody can untoggle. The two
  // destructive ones are reached only through a confirm screen — the panel has
  // one knob and no undo.
  virtual void begin_pairing() {}
  virtual void forget_hosts() {}
  virtual void forget_network() {}

  // Something a host asked the panel to show.
  //
  // Not carried on the command queue: that is four bytes and this has text in
  // it. The device keeps the latest request in the network component and the
  // model collects it here, the same arrangement net_text already uses — with
  // the difference that the model *copies* this rather than holding a pointer
  // into somebody else's buffer.
  virtual bool take_draw(DrawPayload* out) { return false; }
};

}  // namespace ui
