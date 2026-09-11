#include "panel/screens.h"

#include <cmath>

#include "panel/anim.h"
#include "panel/font.h"
#include "panel/icons.h"
#include "panel/mini_font.h"
#include "panel/sprite.h"

namespace panel {
namespace {

constexpr int kTinyAdvance = kTinyW + 1;  // digit plus its gap
constexpr int kColonWidth = 2;

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

namespace {

// A sheen that crosses the label area now and then: enough motion to read as
// alive from across a room, not enough to pull your eye off a monitor.
void sheen(Framebuffer& fb, const Anim& a, int row, int x0, int x1, RGB c, float period_s) {
  const float p = a.phase(period_s);
  if (p > 0.45f) return;  // travels for part of the cycle, then rests
  const float head = x0 + (p / 0.45f) * (x1 - x0 + 4) - 2.0f;
  for (int k = 0; k < 3; ++k) {
    const float w = 1.0f - k * 0.33f;
    fb.set_aa(head - k, row, c, 0.12f * w, Blend::Add);
  }
}

}  // namespace

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

void draw_bar_aa(Framebuffer& fb, int y0, int y1, float fraction, RGB on, RGB off) {
  if (fraction < 0.0f) fraction = 0.0f;
  if (fraction > 1.0f) fraction = 1.0f;
  const float edge = fraction * kWidth;
  for (int y = y0; y <= y1; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      float cov = edge - x;
      if (cov < 0.0f) cov = 0.0f;
      if (cov > 1.0f) cov = 1.0f;
      fb.set(x, y, lerp_rgb(off, on, cov));
    }
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

const Icon& status_icon(Status s) {
  switch (s) {
    case Status::Free: return kIconFree;
    case Status::Busy: return kIconBusy;
    case Status::Dnd: return kIconDnd;
    default: return kIconFree;
  }
}

void draw_screen(Framebuffer& fb, Screen s, const UiState& ui, const Anim& a,
                 ScreenAnim& sa) {
  fb.clear();
  const float dt = a.dt;

  switch (s) {
    case Screen::Status: {
      const RGB base = status_color(ui.status);
      if (!sa.primed) {
        sa.tint.snap(base);
        sa.primed = true;
      }
      sa.tint.set_target(base);
      const RGB c = sa.tint.update(dt);

      // Every status breathes; CALL also animates its icon and pulses harder,
      // because it is the one that must interrupt you.
      const bool urgent = (ui.status == Status::Call);
      const float depth = urgent ? 0.35f : 0.12f;
      const float period = urgent ? 1.6f : 4.0f;
      const float k = (1.0f - depth) + depth * a.wave(period);
      const RGB lit = c.scaled(static_cast<uint8_t>(k * 255.0f + 0.5f));

      // A status change turns the icon and the word over where they stand,
      // rather than dissolving the whole screen. The panel has not moved you
      // anywhere — one fact about it changed — and an element swap says that,
      // where a screen transition says something louder and less true.
      if (!sa.swap.primed) {
        sa.shown = ui.status;
        sa.swap.primed = true;
        sa.swap.t0 = -1000.0;
      } else if (ui.status != sa.shown && !sa.swap.running(a.t)) {
        sa.was = sa.shown;
        sa.shown = ui.status;
        sa.swap.trigger(a.t);
      }
      const float u = sa.swap.u(a.t);

      if (u >= 1.0f) {
        if (urgent) {
          draw_anim_icon(fb, kIconX, 0, kAnimCall, a.t, lit);
        } else {
          draw_icon(fb, kIconX, 0, status_icon(ui.status), lit);
        }
        mini_draw_text_centered(fb, kLabelX, kMiniLabelBox, 1,
                                status_label(ui.status), lit);
      } else {
        // The icon shrinks into itself in both axes; the label only squashes
        // vertically, so the word stays legible for most of the exchange.
        const float out = pop_out_scale(u), in = pop_in_scale(u);
        const RGB was_c = status_color(sa.was);
        if (out > 0.0f) {
          draw_icon_scaled(fb, kIconX, 0, status_icon(sa.was), was_c, out, out, out);
          mini_draw_text_squashed(fb, kLabelX, kMiniLabelBox, 1,
                                  status_label(sa.was), was_c, out, out);
        }
        if (in > 0.0f) {
          const float b = in > 1.0f ? 1.0f : in;
          draw_icon_scaled(fb, kIconX, 0, status_icon(sa.shown), lit, in, in, b);
          mini_draw_text_squashed(fb, kLabelX, kMiniLabelBox, 1,
                                  status_label(sa.shown), lit, in, b);
        }
      }
      sheen(fb, a, 7, kLabelX, kWidth - 1, c, 6.0f);
      break;
    }

    case Screen::Clock: {
      draw_pair_face_anim(fb, sa.face, ui.hour, ui.minute, true, ui.accent, a.t);
      // The seconds hand walks the perimeter: 24+24+6+6 is exactly 60 cells,
      // one per second, and it never crosses the digits.
      const float secs = ui.second + a.phase(1.0f);
      const float pos = secs;
      auto perim = [](float i, float* x, float* y) {
        if (i < 24) { *x = i; *y = 0; }
        else if (i < 30) { *x = 23; *y = i - 24 + 1; }
        else if (i < 54) { *x = 23 - (i - 30); *y = 7; }
        else { *x = 0; *y = 7 - (i - 54) - 1; }
      };
      for (int k = 0; k < 3; ++k) {
        float px, py;
        float i = pos - k;
        if (i < 0) i += 60.0f;
        perim(i, &px, &py);
        fb.set_aa2(px, py, ui.accent, k == 0 ? 1.0f : 0.25f / k, Blend::Add);
      }
      break;
    }

    case Screen::Timer: {
      const int left = ui.timer_left_s < 0 ? 0 : ui.timer_left_s;
      RGB c = (left <= 60) ? RGB(255, 40, 20) : ui.accent;
      // Under a minute the face pulses, and faster in the last ten seconds.
      if (left <= 60) {
        const float rate = (left <= 10) ? 0.5f : 1.0f;
        c = c.scaled(static_cast<uint8_t>((0.55f + 0.45f * a.wave(rate)) * 255.0f));
      }
      // A paused timer throbs rather than hard-blinking, so it still reads.
      if (!ui.timer_running) {
        c = c.scaled(static_cast<uint8_t>((0.45f + 0.35f * a.wave(1.2f)) * 255.0f));
      }
      draw_pair_face_anim(fb, sa.face, left / 60, left % 60, true, c, a.t);

      const float done = ui.timer_total_s > 0
                             ? 1.0f - static_cast<float>(left) / ui.timer_total_s
                             : 0.0f;
      if (!sa.primed) { sa.bar.snap(done); sa.primed = true; }
      sa.bar.set_target(done);
      const float shown = sa.bar.update(dt);
      draw_bar_aa(fb, kHeight - 1, kHeight - 1, shown, c.scaled(120), RGB(0, 0, 0));
      // A brighter head on the bar, so progress reads even when it barely moves.
      fb.set_aa(shown * kWidth - 0.5f, kHeight - 1, c, 0.8f, Blend::Add);
      break;
    }

    case Screen::Brightness: {
      const float frac = ui.brightness / 255.0f;
      if (!sa.primed) { sa.bar.snap(frac); sa.rays.snap(frac); sa.primed = true; }
      sa.bar.set_target(frac);
      sa.rays.set_target(frac);
      const float shown = sa.bar.update(dt);
      const float ray = sa.rays.update(dt);

      draw_icon(fb, kIconX, 0, kIconSunCore, ui.accent);
      // The rays are the readout: they extend with the value.
      for (int row = 0; row < kIconH; ++row) {
        for (int col = 0; col < kIconW; ++col) {
          if (kIconSunRays.rows[row] & (1 << (kIconW - 1 - col))) {
            fb.blend(kIconX + col, row, ui.accent, ray);
          }
        }
      }
      // A glint orbits the ray tips.
      const float g = a.phase(3.0f) * 4.0f;
      const int gi = static_cast<int>(g) & 3;
      const int gx[4] = {3, 7, 4, 0}, gy[4] = {0, 3, 7, 4};
      fb.set(kIconX + gx[gi], gy[gi], RGB(255, 255, 255));

      const int pct = (ui.brightness * 100 + 127) / 255;
      char buf[5];
      int n = 0;
      if (pct >= 100) buf[n++] = static_cast<char>('0' + pct / 100);
      if (pct >= 10) buf[n++] = static_cast<char>('0' + (pct / 10) % 10);
      buf[n++] = static_cast<char>('0' + pct % 10);
      buf[n++] = '%';
      buf[n] = 0;
      mini_draw_text_centered(fb, kLabelX, kMiniLabelBox, 1, buf, ui.accent);
      draw_bar_aa(fb, 7, 7, shown, ui.accent, ui.accent.scaled(14));
      break;
    }

    case Screen::ColorPick: {
      for (int x = 0; x < kWidth; ++x) {
        const RGB c = hsv(static_cast<float>(x) / kWidth, 1.0f, 1.0f);
        for (int y = 0; y <= 4; ++y) fb.set(x, y, c);
      }
      // A specular band travels the ramp so it never looks like a static image.
      const float band = a.phase(5.0f) * (kWidth + 6) - 3.0f;
      for (int k = -2; k <= 2; ++k) {
        fb.set_aa(band + k, 2, RGB(255, 255, 255), 0.22f - 0.07f * (k < 0 ? -k : k),
                  Blend::Add);
      }
      // The selected colour, breathing, under the ramp.
      const RGB sel = hsv(ui.hue, 1.0f, 1.0f);
      const float br = 0.7f + 0.3f * a.wave(4.0f);
      for (int x = 0; x < kWidth; ++x) {
        fb.set(x, 6, sel.scaled(static_cast<uint8_t>(br * 255.0f)));
      }
      // Map onto kWidth-1, not kWidth: at hue 1.0 a cursor at column 24 is
      // off the panel and draws nothing. The cursor has to reach both ends of
      // the ramp it is selecting from.
      const float cursor_x = clamp01(ui.hue) * (kWidth - 1);
      if (!sa.primed) { sa.cursor.snap(cursor_x); sa.primed = true; }
      sa.cursor.set_target(cursor_x);
      const float cx = sa.cursor.update(dt);
      const float pulse_k = 0.7f + 0.3f * a.wave(1.5f);
      fb.set_aa(cx, 7, RGB(255, 255, 255), pulse_k, Blend::Add);
      break;
    }

    case Screen::TimerSet: {
      const int mins = ui.timer_set_min < 0 ? 0 : ui.timer_set_min;
      const float frac = mins / 60.0f;
      if (!sa.primed) { sa.bar.snap(frac); sa.rays.snap(frac); sa.primed = true; }
      sa.bar.set_target(frac);
      sa.rays.set_target(frac);
      const float shown = sa.bar.update(dt);
      draw_hourglass(fb, kIconX, 0, sa.rays.update(dt), a.t, ui.accent.scaled(120),
                     ui.accent);
      char buf[5];
      int n = 0;
      if (mins >= 10) buf[n++] = static_cast<char>('0' + mins / 10);
      buf[n++] = static_cast<char>('0' + mins % 10);
      buf[n++] = 'M';
      buf[n] = 0;
      mini_draw_text_centered(fb, kLabelX, kMiniLabelBox, 1, buf, ui.accent);
      draw_bar_aa(fb, 7, 7, shown, ui.accent, ui.accent.scaled(14));
      break;
    }

    case Screen::Sleep: {
      // Dim, but not so dim it disappears: below a framebuffer value of about
      // 60 the gamma curve and the default brightness together round the
      // output to zero, so the old 2..10 breathe never lit an LED at all.
      const float k = 0.35f + 0.65f * a.wave(6.0f);
      const uint8_t v = static_cast<uint8_t>(70 + 50 * k);
      const float drift = a.phase(60.0f) * kWidth;
      draw_icon_aa(fb, drift - 4.0f, 0.0f, kIconMoon, RGB(v / 3, v / 3, v / 2));
      draw_icon_aa(fb, drift - 4.0f + kWidth, 0.0f, kIconMoon, RGB(v / 3, v / 3, v / 2));
      break;
    }

    case Screen::Booting: {
      const RGB c = ui.wifi_connected ? RGB(0, 190, 80) : RGB(60, 120, 220);
      if (ui.wifi_connected) {
        draw_stroke_icon(fb, kIconX, 0, kStrokeCheck,
                         clamp01(static_cast<float>(a.t) * 2.0f), c, RGB(255, 255, 255));
        mini_draw_text_centered(fb, kLabelX, kMiniLabelBox, 1, "OK", c);
      } else {
        draw_anim_icon(fb, kIconX, 0, kAnimWifi, a.t, c);
        mini_draw_text_centered(fb, kLabelX, kMiniLabelBox, 1, "WIFI", c);
        sheen(fb, a, 7, kLabelX, kWidth - 1, c, 1.5f);
      }
      break;
    }

    default:
      break;
  }
}

void draw_screen(Framebuffer& fb, Screen s, const UiState& ui, uint32_t now_ms) {
  // Still form for callers that have no frame clock. The scratch state is
  // local, not static: each call starts fresh so eased values snap to their
  // target rather than carrying a half-finished glide in from a previous call
  // with completely different state.
  ScreenAnim scratch;
  Anim a;
  a.dt = 0.0f;
  a.t = now_ms / 1000.0;
  draw_screen(fb, s, ui, a, scratch);
}

}  // namespace panel
