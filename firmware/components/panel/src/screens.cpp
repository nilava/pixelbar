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
    case Status::Away: return "AWAY";
    case Status::Focus: return "FOCUS";
    case Status::Lunch: return "LUNCH";
    // MEETING measures 27 px and will not fit the panel at all, let alone the
    // label box. Shortened rather than scrolled.
    case Status::Meet: return "MEET";
    default: return "?";
  }
}

RGB status_color(Status s) {
  switch (s) {
    case Status::Free: return RGB(0, 210, 70);
    case Status::Busy: return RGB(255, 30, 15);
    case Status::Call: return RGB(255, 120, 0);
    case Status::Dnd: return RGB(190, 0, 130);
    case Status::Away: return RGB(60, 110, 210);
    case Status::Focus: return RGB(140, 60, 255);
    case Status::Lunch: return RGB(255, 160, 30);
    case Status::Meet: return RGB(0, 175, 185);
    default: return RGB(120, 120, 120);
  }
}

namespace {

// A play triangle, three columns by five rows. Small because the time face
// already owns columns 3 to 19, and there is exactly this much either side.
const uint8_t kPlayRows[5] = {0b100, 0b110, 0b111, 0b110, 0b100};

// The "press me" affordance, inflating and deflating where it stands.
//
// Copied from frame-stepping the real thing, where the arrow beside a stopped
// timer grows wider and brighter and then contracts on about a 0.7 s cycle. It
// is the one piece of motion on the screen that is about what you could do
// rather than about what is happening, and a stopped timer without it just
// looks like a stopped clock.
void draw_play_hint(Framebuffer& fb, const Anim& a, RGB c) {
  const Sprite play{kPlayRows, 3, 5, 2};
  const float k = a.wave(0.7f);
  const float sx = 0.55f + 0.45f * k;          // it inflates toward the face
  const float bright = 0.45f + 0.55f * k;
  draw_sprite_scaled(fb, play, 1.5f, 3.5f, sx, 1.0f, c, bright);
}

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
    case Screen::Menu: return "menu";
    case Screen::StatusPick: return "statuspick";
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
    // CALL animates when it is settled, but a swap needs one still frame to
    // scale, and falling through to the FREE ring here meant a change into or
    // out of CALL turned over the wrong picture.
    case Status::Call: return kAnimCall.frames[0];
    default: return kIconFree;
  }
}

RGB accent_from_hue(float hue) { return hsv(clamp01(hue), 1.0f, 1.0f); }

bool status_uses_badge(Status s) { return !mini_text_fits(status_label(s)); }

// Every label clears the 15 px box beside the icon, so none of them scroll.
// Four characters is not the rule, though — M and W are five columns wide in
// this font, so MOVE measures 17 and does not fit while APPS at four does.
// TILT and TAP are the versions that fit, and a test checks the whole table
// rather than trusting the next entry to be measured by hand.
// One entry, one destination. No chaining and no submenus.
//
// An earlier version had a DISP entry that opened brightness and then stepped
// on to colour and then to the timer length when you pressed again — three
// unrelated settings behind one row, reached in a fixed order, with no way back
// except all the way home. Four rows that each go exactly one place is both
// smaller and easier to explain.
//
// The icons for apps, motion, touch and network are drawn and waiting in
// icons.h. They arrive with the features behind them: a row that does nothing
// when you press it teaches you that pressing does not work.
const MenuEntry kMenu[] = {
    {&kIconBusy, "STAT", RGB(255, 30, 15), false},
    {&kIconHourglass, "TIME", RGB(255, 138, 31), false},
    {&kIconSunCore, "DIM", RGB(255, 200, 60), false},
    {&kIconPalette, "HUE", RGB(140, 60, 255), false},
};
const int kMenuCount = static_cast<int>(sizeof(kMenu) / sizeof(kMenu[0]));

void draw_badge(Framebuffer& fb, int x0, int y0, int w, int h, RGB fill) {
  if (w <= 0 || h <= 0) return;
  for (int y = y0; y < y0 + h; ++y) {
    for (int x = x0; x < x0 + w; ++x) {
      // Drop the four corners. At eight rows that is the whole of what a
      // corner radius can express, and it is enough: a square-cornered block
      // reads as a rendering mistake, a clipped one reads as a shape.
      const bool corner = (x == x0 || x == x0 + w - 1) && (y == y0 || y == y0 + h - 1);
      if (corner) continue;
      fb.set(x, y, fill);
    }
  }
}

void draw_badge_aa(Framebuffer& fb, float top, float bottom, RGB fill) {
  if (bottom <= top) return;
  const int first = static_cast<int>(std::floor(top));
  const int last = static_cast<int>(std::ceil(bottom)) - 1;
  for (int y = 0; y < kHeight; ++y) {
    const float lo = top > static_cast<float>(y) ? top : static_cast<float>(y);
    const float hi = bottom < static_cast<float>(y + 1) ? bottom : static_cast<float>(y + 1);
    const float cov = hi - lo;
    if (cov <= 0.0f) continue;
    for (int x = 0; x < kWidth; ++x) {
      const bool corner = (x == 0 || x == kWidth - 1) && (y == first || y == last);
      if (corner) continue;
      fb.blend(x, y, fill, cov);
    }
  }
}

void draw_badge_label(Framebuffer& fb, int y0, int h, const char* s, RGB fill) {
  draw_badge(fb, 0, y0, kWidth, h, fill);
  // The word is cut out rather than drawn on: black glyphs written over the
  // fill with set(), not add(). Centred in the whole panel, because with the
  // badge there is no icon competing for the left eight columns.
  const int ty = y0 + (h - kMiniH) / 2;
  mini_draw_text_centered(fb, 0, kWidth, ty, s, RGB(0, 0, 0));
}


// Draws one status at a given vertical scale and brightness, in whichever
// layout its label needs.
//
// Two layouts have to be able to turn over into each other, because BUSY uses
// an icon and FOCUS uses a badge, and you can go straight from one to the
// other. Keeping them behind one call is what makes that a non-event: the swap
// code never learns which is which.
void draw_status_face(Framebuffer& fb, Status st, RGB c, const Anim& a, float sy,
                      float bright, bool animate) {
  if (sy <= 0.0f || bright <= 0.0f) return;
  const char* label = status_label(st);

  if (status_uses_badge(st)) {
    // The badge collapses about the same centre its label does, so the two
    // stay locked together all the way down.
    const float mid = kBadgeTop + kBadgeRows * 0.5f;
    const float half = kBadgeRows * 0.5f * sy;
    draw_badge_aa(fb, mid - half, mid + half,
                  c.scaled(static_cast<uint8_t>(bright * kBadgeFill * 255.0f + 0.5f)));
    mini_draw_text_squashed(fb, 0, kWidth, kBadgeTextY, label, RGB(0, 0, 0), sy,
                            1.0f, Blend::Over);
    return;
  }

  if (sy >= 0.999f && bright >= 0.999f) {
    if (animate && st == Status::Call) {
      draw_anim_icon(fb, kIconX, 0, kAnimCall, a.t, c);
    } else {
      draw_icon(fb, kIconX, 0, status_icon(st), c);
    }
    mini_draw_text_centered(fb, kLabelX, kMiniLabelBox, 1, label, c);
    return;
  }
  // The icon shrinks in both axes; the label only squashes vertically, so the
  // word stays legible for longer than the picture does.
  draw_icon_scaled(fb, kIconX, 0, status_icon(st), c, sy, sy, bright);
  mini_draw_text_squashed(fb, kLabelX, kMiniLabelBox, 1, label, c, sy, bright);
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
        draw_status_face(fb, ui.status, lit, a, 1.0f, 1.0f, true);
      } else {
        const float out = pop_out_scale(u), in = pop_in_scale(u);
        if (out > 0.0f)
          draw_status_face(fb, sa.was, status_color(sa.was), a, out, out, false);
        if (in > 0.0f) {
          const float b = in > 1.0f ? 1.0f : in;
          draw_status_face(fb, sa.shown, lit, a, in, b, false);
        }
      }
      // The sheen travels the label area, which only exists in the icon
      // layout. Over a filled badge it would read as a smudge on the colour.
      if (!status_uses_badge(ui.status)) sheen(fb, a, 7, kLabelX, kWidth - 1, c, 6.0f);
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
      if (!ui.timer_running && left > 0) draw_play_hint(fb, a, ui.accent);
      break;
    }

    case Screen::Menu: {
      const int n = kMenuCount;
      int idx = ui.menu_index;
      if (idx < 0 || idx >= n) idx = 0;
      const MenuEntry& e = kMenu[idx];

      // The item breathes gently so a menu left open does not look frozen, and
      // the one icon that means "machinery" turns while you look at it. That is
      // the difference between an icon and a picture of an icon.
      const float k = 0.86f + 0.14f * a.wave(3.2f);
      const RGB lit = e.color.scaled(static_cast<uint8_t>(k * 255.0f + 0.5f));
      if (e.spins) {
        draw_sprite_rotated(fb, sprite_of(*e.icon), kIconX + kIconW * 0.5f,
                            kIconH * 0.5f, a.phase(6.0f), lit, 1.0f, 1.0f,
                            Blend::Add, kIconX, kIconX + kIconW);
      } else {
        draw_icon(fb, kIconX, 0, *e.icon, lit);
      }
      // Grey rather than white. A label lights all three channels, and at 220
      // the brightest entries went over the 2500 mA cap on their own — which
      // would have dimmed some menu rows and not others as you scrolled.
      mini_draw_text_centered(fb, kLabelX, kMiniLabelBox, 1, e.label,
                              RGB(140, 140, 140));

      // Where you are in the list, as ticks along the bottom row. At eight
      // entries and fifteen columns of label there is no room for a number, and
      // a position you can see at a glance beats one you have to read.
      const int span = kWidth - kLabelX;
      for (int i = 0; i < n && i < span; ++i) {
        const int x = kLabelX + (i * span) / n;
        fb.set(x, 7, i == idx ? lit : e.color.scaled(30));
      }
      break;
    }

    case Screen::StatusPick: {
      // Drawn as the status itself rather than as a row about it, so what you
      // are scrolling through is a preview of what the room will see. The
      // badge layouts and the icon layouts sit in one list without the picker
      // needing to know which is which.
      const RGB c = status_color(ui.pick);
      const float k = 0.88f + 0.12f * a.wave(2.6f);
      draw_status_face(fb, ui.pick, c.scaled(static_cast<uint8_t>(k * 255.0f + 0.5f)),
                       a, 1.0f, 1.0f, true);
      // A cursor along the bottom, in the same place the menu puts one.
      const int n = static_cast<int>(Status::Count);
      const int here = static_cast<int>(ui.pick);
      if (!status_uses_badge(ui.pick)) {
        for (int i = 0; i < n && i < kWidth; ++i) {
          const int x = (i * kWidth) / n;
          fb.set(x, 7, i == here ? c : c.scaled(26));
        }
      }
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
      // 80..130 rather than 70..120: at the default brightness of 48 the
      // bottom of the old range rendered to 3 of 255, which is a moon you have
      // to hunt for in a dark room.
      const uint8_t v = static_cast<uint8_t>(80 + 50 * k);
      // The moon is cool rather than white, but the tint is applied by holding
      // the *brightest* channel at v and pulling the others down — not by
      // scaling all three. Dividing every channel by three, as this did, put
      // the whole thing back under the floor the line above exists to clear,
      // and the screen went nearly black again while the comment still claimed
      // otherwise.
      const RGB moon(static_cast<uint8_t>(v * 2 / 3), static_cast<uint8_t>(v * 2 / 3), v);
      const float drift = a.phase(60.0f) * kWidth;
      draw_icon_aa(fb, drift - 4.0f, 0.0f, kIconMoon, moon);
      draw_icon_aa(fb, drift - 4.0f + kWidth, 0.0f, kIconMoon, moon);
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
