// WiFi and the web page, and the queue between them and the render loop.
//
// The one rule this component exists to honour: nothing here touches the model
// directly. An HTTP handler runs on the server's task, the model runs on the
// render loop, and the only thing that crosses between them is a twelve-byte
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
} net_status_t;

// Brings up WiFi and, once it has an address, the HTTP server. Returns as soon
// as the attempt has started: the render loop must not wait for a network.
esp_err_t net_start(void);

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
