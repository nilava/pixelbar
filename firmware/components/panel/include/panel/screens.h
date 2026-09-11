// The screens the UI shows, as opposed to the ambient patterns.
//
// Every screen is a pure function of UiState and the clock, so the input layer
// and the state machine can be developed and tested separately from drawing.
#pragma once
#include <cstdint>

#include "panel/color.h"
#include "panel/framebuffer.h"

namespace panel {

enum class Status : uint8_t { Free, Busy, Call, Dnd, Count };

const char* status_label(Status s);
RGB status_color(Status s);

enum class Screen : uint8_t {
  Status,      // the room-facing default: one word, one colour
  Clock,
  Timer,       // focus countdown
  Brightness,  // encoder adjust
  ColorPick,   // encoder adjust
  TimerSet,    // encoder adjust
  Sleep,
  Booting,
  Count,
};

const char* screen_name(Screen s);

struct UiState {
  Status status = Status::Free;
  RGB accent{255, 138, 31};

  // Focus timer.
  int timer_total_s = 25 * 60;
  int timer_left_s = 25 * 60;
  bool timer_running = false;

  // Adjustable settings.
  uint8_t brightness = 48;
  float hue = 0.08f;      // cursor position on the colour picker, 0..1
  int timer_set_min = 25;

  // Wall clock and link state, filled in by the caller.
  int hour = 0;
  int minute = 0;
  int second = 0;
  bool wifi_connected = false;
};

void draw_screen(Framebuffer& fb, Screen s, const UiState& ui, uint32_t now_ms);

// Layout helpers, shared with the pattern set and exercised by the tests.

// Two 2-digit groups separated by a colon, centred. Each group is clamped to
// 0..99, never wrapped, so a 25 minute timer reads 25 and not 01.
void draw_pair_face(Framebuffer& fb, int left, int right, bool show_colon, RGB color);

// hh:mm in 3x5 digits, centred, with the colon shown when show_colon is set.
void draw_clock_face(Framebuffer& fb, int hour, int minute, bool show_colon, RGB color);

// A horizontal bar across rows y0..y1 inclusive, filled left to right.
void draw_bar(Framebuffer& fb, int y0, int y1, float fraction, RGB on, RGB off);

// Right-aligned fixed-width number in 3x5 digits. Leading zeros are dropped.
void draw_tiny_number(Framebuffer& fb, int x, int y, int value, int digits, RGB color);
int tiny_number_width(int digits);

}  // namespace panel
