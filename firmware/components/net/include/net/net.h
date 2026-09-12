// WiFi and the web page, and the queue between them and the render loop.
//
// The one rule this component exists to honour: nothing here touches the model
// directly. An HTTP handler runs on the server's task, the model runs on the
// render loop, and the only thing that crosses between them is a four-byte
// command through a FreeRTOS queue that the loop drains at the top of a frame.
//
// It matters more than it looks. A handler that reached into the App would be
// mutating state halfway through a frame that is already being drawn from it,
// and the failure would be a rare torn frame rather than anything a test would
// catch.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  NET_CMD_NONE = 0,
  // The touch pads, as pad levels rather than as events: `a` is the zone.
  // A web button and a finger are the same thing from the recogniser up, which
  // is the whole point — see ui::VirtualPads.
  NET_CMD_TAP,
  NET_CMD_HOLD,     // latches down
  NET_CMD_RELEASE,  // and up
  NET_CMD_SWIPE,    // a = +1 rightward, -1 leftward
  // The encoder, likewise: detents and switch levels, not gestures.
  NET_CMD_TURN,     // a = detents, signed
  NET_CMD_PRESS,
  NET_CMD_PRESS_HOLD,
  // And the two things worth setting directly rather than by gesture.
  NET_CMD_STATUS,      // a = panel::Status
  NET_CMD_BRIGHTNESS,  // a = 0..255
} net_cmd_kind_t;

typedef struct {
  uint8_t kind;
  int16_t a;
} net_cmd_t;

// What the page shows. Written by the render loop once a frame, read by the
// server's task. A torn read would at worst mix two frames of a status
// display, so this is deliberately not locked.
typedef struct {
  char screen[16];
  char status[8];
  uint8_t brightness;
  int timer_left_s;
  bool timer_running;
  float fps;
  int32_t detents;
  uint32_t illegal;
  // Where the panel is in whatever list it is showing, and what the setting
  // under the cursor reads as. Both are for seeing the panel from somewhere
  // other than in front of it — which is the only way to tell a menu that did
  // not move from one that moved and came back.
  int16_t menu_index;
  char set_label[8];
  char set_text[16];
} net_status_t;

// What the device is doing about the network. The panel draws a different
// screen for each, and they are the only three states worth distinguishing:
// either you need to be told how to set it up, or it is working on it, or it
// is done.
typedef enum {
  NET_MODE_SETUP,    // no stored credentials: the setup AP is up
  NET_MODE_JOINING,  // credentials stored, trying to associate
  NET_MODE_ONLINE,   // associated, with an address
} net_mode_t;

// Brings up WiFi and, once it has an address, the HTTP server. Returns as soon
// as the attempt has started: the render loop must not wait for a network.
esp_err_t net_start(void);

net_mode_t net_mode(void);
// The setup network's name, e.g. "PIXELBAR-A3F2". Empty unless in setup mode;
// this is what the panel tells you to join.
const char* net_setup_ssid(void);
// What the panel should display for the current mode: the setup network to
// join, the network being tried, or the address. Never null.
const char* net_panel_text(void);

// -1 when no update is in flight, otherwise 0..1. The panel shows a progress
// screen whenever this is not negative.
float net_ota_progress(void);

// The Bluetooth pairing code the panel should be showing, or 0. Six digits
// that exist nowhere else, which is what makes a bond mean the host is in the
// room rather than merely within radio range.
uint32_t net_passkey(void);

// The latest thing a host asked the panel to show.
//
// Text does not fit in the four-byte command queue, so this crosses the seam
// the same way the network screens' text does: the network component keeps it
// and the render loop collects it. Returns false when nothing new has arrived
// since the last call, so the model can tell a fresh request from the same one
// sitting there.
typedef struct {
  char text[48];
  char icon[12];
  char source[16];
  uint8_t priority;
  float ttl_s;
  int64_t until_unix;
  float bar;
  uint32_t tint;
} net_draw_t;

bool net_take_draw(net_draw_t* out);

// A copy of what the panel is showing, as 192 RGB triples in row-major order.
//
// The point is to stop debugging this device by photographing it. Called from
// the render loop; rate-limited inside, because a mirror nobody is watching
// should cost nothing.
void net_publish_frame(const uint8_t* rgb, int count);

// Confirms the running image so the bootloader stops holding the previous one
// in reserve. Call it only once the device has demonstrably survived: doing it
// at startup would defeat the point of rollback entirely.
void net_mark_healthy(void);

// True once there is an address. What the panel shows while there is not.
bool net_connected(void);
// Dotted quad, or an empty string. Valid once connected.
const char* net_ip(void);

// True once the clock has been set from the network at least once. False means
// there is no time at all, and the panel must say so rather than draw a zero.
bool net_time_valid(void);

// Called from the render loop each frame.
void net_publish(const net_status_t* s);
// Drains one command, or returns false. Never blocks.
bool net_take_cmd(net_cmd_t* out);

#ifdef __cplusplus
}
#endif
