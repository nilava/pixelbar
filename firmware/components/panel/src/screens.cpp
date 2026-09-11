#include "panel/screens.h"

#include <cmath>

#include "panel/font.h"

namespace panel {
namespace {

constexpr int kTinyAdvance = kTinyW + 1;  // digit plus its gap
constexpr int kColonWidth = 2;

// 0..1 triangle-free sine, used for gentle pulses.
float pulse(uint32_t now_ms, float period_s) {
  const float t = static_cast<float>(now_ms % static_cast<uint32_t>(period_s * 1000.0f));
  return 0.5f + 0.5f * std::sin(t / (period_s * 1000.0f) * 6.28318f);
}

}  // namespace

const char* status_label(Status s) {
  switch (s) {
    case Status::Free: return "FREE";
    case Status::Busy: return "BUSY";
    case Status::Call: return "CALL";
    case Status::Dnd: return "DND";
    default: return "?";
  }
}

RGB status_color(Status s) {
  switch (s) {
    case Status::Free: return RGB(0, 210, 70);
    case Status::Busy: return RGB(255, 30, 15);
    case Status::Call: return RGB(255, 120, 0);
    case Status::Dnd: return RGB(190, 0, 130);
    default: return RGB(120, 120, 120);
  }
}

const char* screen_name(Screen s) {
  switch (s) {
    case Screen::Status: return "status";
    case Screen::Clock: return "clock";
    case Screen::Timer: return "timer";
    case Screen::Brightness: return "brightness";
    case Screen::ColorPick: return "colorpick";
    case Screen::TimerSet: return "timerset";
    case Screen::Sleep: return "sleep";
    case Screen::Booting: return "booting";
    default: return "?";
  }
}

int tiny_number_width(int digits) { return digits * kTinyAdvance - 1; }

void draw_tiny_number(Framebuffer& fb, int x, int y, int value, int digits, RGB color) {
  if (value < 0) value = 0;
  for (int i = digits - 1; i >= 0; --i) {
    const int d = value % 10;
    value /= 10;
    // Drop leading zeros, but always keep the last digit.
    const bool blank = (d == 0 && value == 0 && i < digits - 1 && i > 0);
    if (!blank || i == digits - 1 || value > 0 || d != 0) {
      draw_tiny_digit(fb, x + i * kTinyAdvance, y, static_cast<char>('0' + d), color);
    }
  }
}

void draw_bar(Framebuffer& fb, int y0, int y1, float fraction, RGB on, RGB off) {
  if (fraction < 0.0f) fraction = 0.0f;
  if (fraction > 1.0f) fraction = 1.0f;
  const int lit = static_cast<int>(fraction * kWidth + 0.5f);
  for (int y = y0; y <= y1; ++y) {
    for (int x = 0; x < kWidth; ++x) fb.set(x, y, x < lit ? on : off);
  }
}

void draw_pair_face(Framebuffer& fb, int left, int right, bool show_colon, RGB color) {
  // Each group is clamped, not wrapped: a 25 minute timer must read 25, and
  // wrapping it into a 24 hour clock would show 01.
  auto clamp99 = [](int v) { return v < 0 ? 0 : (v > 99 ? 99 : v); };
  const int a = clamp99(left);
  const int b = clamp99(right);
  // 4 digits at 4 px each plus a 2 px colon = 18 px, centred in 24.
  int x = (kWidth - (4 * kTinyAdvance + kColonWidth - 1)) / 2;
  const int y = 1;
  draw_tiny_digit(fb, x, y, static_cast<char>('0' + a / 10), color); x += kTinyAdvance;
  draw_tiny_digit(fb, x, y, static_cast<char>('0' + a % 10), color); x += kTinyAdvance;
  if (show_colon) {
    fb.set(x, y + 1, color);
    fb.set(x, y + 3, color);
  }
  x += kColonWidth;
  draw_tiny_digit(fb, x, y, static_cast<char>('0' + b / 10), color); x += kTinyAdvance;
  draw_tiny_digit(fb, x, y, static_cast<char>('0' + b % 10), color);
}

void draw_clock_face(Framebuffer& fb, int hour, int minute, bool show_colon, RGB color) {
  draw_pair_face(fb, ((hour % 24) + 24) % 24, ((minute % 60) + 60) % 60, show_colon, color);
}

void draw_screen(Framebuffer& fb, Screen s, const UiState& ui, uint32_t now_ms) {
  fb.clear();
  switch (s) {
    case Screen::Status: {
      RGB c = status_color(ui.status);
      // CALL pulses gently: it is the one status that means "not even a quick
      // question", and movement carries further than colour alone.
      if (ui.status == Status::Call) {
        const float k = 0.65f + 0.35f * pulse(now_ms, 2.0f);
        c = c.scaled(static_cast<uint8_t>(k * 255.0f));
      }
      draw_text_centered(fb, 0, status_label(ui.status), c);
      break;
    }

    case Screen::Clock:
      draw_clock_face(fb, ui.hour, ui.minute, ui.second % 2 == 0, ui.accent);
      draw_bar(fb, kHeight - 1, kHeight - 1, ui.second / 60.0f, ui.accent.scaled(90),
               RGB(0, 0, 0));
      break;

    case Screen::Timer: {
      const int left = ui.timer_left_s < 0 ? 0 : ui.timer_left_s;
      // Under a minute the whole face turns red: that is the cue to wrap up.
      RGB c = (left <= 60) ? RGB(255, 40, 20) : ui.accent;
      // A paused timer blinks so it is never mistaken for a running one. The
      // dim phase stays readable rather than going dark, so a paused timer
      // still shows the time at a glance.
      if (!ui.timer_running && (now_ms / 500) % 2 == 0) c = c.scaled(70);
      draw_pair_face(fb, left / 60, left % 60, true, c);
      const float done = ui.timer_total_s > 0
                             ? 1.0f - static_cast<float>(left) / ui.timer_total_s
                             : 0.0f;
      draw_bar(fb, kHeight - 1, kHeight - 1, done, c.scaled(110), RGB(0, 0, 0));
      break;
    }

    case Screen::Brightness: {
      const int pct = (ui.brightness * 100 + 127) / 255;
      const int digits = pct >= 100 ? 3 : (pct >= 10 ? 2 : 1);
      const int x = (kWidth - tiny_number_width(digits)) / 2;
      draw_tiny_number(fb, x, 0, pct, digits, ui.accent);
      draw_bar(fb, 6, 7, ui.brightness / 255.0f, ui.accent, ui.accent.scaled(12));
      break;
    }

    case Screen::ColorPick: {
      // A hue ramp you scrub with the knob, with the cursor under the choice.
      for (int x = 0; x < kWidth; ++x) {
        const RGB c = hsv(static_cast<float>(x) / kWidth, 1.0f, 1.0f);
        for (int y = 0; y <= 5; ++y) fb.set(x, y, c);
      }
      int cursor = static_cast<int>(ui.hue * kWidth + 0.5f);
      if (cursor < 0) cursor = 0;
      if (cursor >= kWidth) cursor = kWidth - 1;
      fb.set(cursor, 7, RGB(255, 255, 255));
      if (cursor > 0) fb.set(cursor - 1, 7, RGB(60, 60, 60));
      if (cursor < kWidth - 1) fb.set(cursor + 1, 7, RGB(60, 60, 60));
      break;
    }

    case Screen::TimerSet: {
      const int mins = ui.timer_set_min < 0 ? 0 : ui.timer_set_min;
      const int digits = mins >= 10 ? 2 : 1;
      const int x = (kWidth - tiny_number_width(digits)) / 2;
      draw_tiny_number(fb, x, 0, mins, digits, ui.accent);
      draw_bar(fb, 6, 7, mins / 60.0f, ui.accent, ui.accent.scaled(12));
      break;
    }

    case Screen::Sleep: {
      // One dim breathing pixel in the bottom corner: enough to show it is
      // alive, dark enough to ignore in a dim room.
      const uint8_t v = static_cast<uint8_t>(2 + 8 * pulse(now_ms, 4.0f));
      fb.set(0, kHeight - 1, RGB(v, v, v));
      break;
    }

    case Screen::Booting: {
      const RGB c = ui.wifi_connected ? RGB(0, 160, 60) : RGB(40, 90, 180);
      draw_text_centered(fb, 0, "WIFI", c);
      // A scanner on the bottom row while it is still trying.
      if (!ui.wifi_connected) {
        const int span = 2 * kWidth - 2;
        int p = static_cast<int>((now_ms / 40) % span);
        if (p >= kWidth) p = span - p;
        fb.set(p, kHeight - 1, c);
      }
      break;
    }

    default:
      break;
  }
}

}  // namespace panel
