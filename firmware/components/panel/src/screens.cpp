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
// A specular highlight sweeping across whatever is already lit.
//
// It used to run along row 7 and add light wherever it went. Row 7 under the
// label is empty, so what it actually produced was a single dim pixel
// crawling from one edge to the other every six seconds on an otherwise dark
// row — which does not read as a highlight at all, it reads as a fault, and
// was reported as one.
//
// The mistake was adding light to nothing. A sheen is what a surface does
// with light, so it only brightens cells that already have ink in them and
// leaves empty ones alone. That also means it can sweep the whole label
// rather than a row beside it, which is where a highlight belongs.
void sheen(Framebuffer& fb, const Anim& a, int y0, int y1, int x0, int x1, RGB c,
           float period_s) {
  const float p = a.phase(period_s);
  if (p > 0.45f) return;  // travels for part of the cycle, then rests
  const float head = x0 + (p / 0.45f) * (x1 - x0 + 4) - 2.0f;
  for (int k = 0; k < 3; ++k) {
    const float w = 1.0f - k * 0.33f;
    const int x = static_cast<int>(head - k + 0.5f);
    if (x < x0 || x > x1) continue;
    for (int y = y0; y <= y1; ++y) {
      const RGB under = fb.get(x, y);
      if (under.r == 0 && under.g == 0 && under.b == 0) continue;
      fb.set_aa(static_cast<float>(x), y, c, 0.45f * w, Blend::Add);
    }
  }
}

}  // namespace

const char* screen_name(Screen s) {
  switch (s) {
    case Screen::Status: return "status";
    case Screen::Clock: return "clock";
    case Screen::Timer: return "timer";
    case Screen::Menu: return "menu";
    case Screen::Group: return "group";
    case Screen::Setting: return "setting";
    case Screen::Scene: return "scene";
    case Screen::StatusPick: return "statuspick";
    case Screen::Brightness: return "brightness";
    case Screen::ColorPick: return "colorpick";
    case Screen::TimerSet: return "timerset";
    case Screen::Sleep: return "sleep";
    case Screen::Booting: return "booting";
    case Screen::WifiSetup: return "wifisetup";
    case Screen::WifiConnecting: return "wificonnecting";
    case Screen::WifiInfo: return "wifiinfo";
    case Screen::OtaProgress: return "ota";
    case Screen::Pairing: return "pairing";
    case Screen::Draw: return "draw";
    case Screen::Paired: return "paired";
    case Screen::Confirm: return "confirm";
    default: return "?";
  }
}

// The scenes, in picker order.
//
// Not every Pattern is a scene. Text and Clock duplicate screens that already
// exist and would be two ways to reach the same picture; MapTest is a wiring
// diagnostic that belongs under a menu nobody browses. What is left is the
// four that are worth looking at.
namespace {
const Pattern kScenes[] = {Pattern::Solid, Pattern::Rainbow, Pattern::Plasma,
                           Pattern::Sparkle};
const char* const kSceneNames[] = {"SOLID", "RAINBOW", "PLASMA", "SPARKLE"};
constexpr int kSceneCount = static_cast<int>(sizeof(kScenes) / sizeof(kScenes[0]));
}  // namespace

// The icons a host may ask for, by name.
//
// A deliberately short list, and deliberately not every icon in the set: these
// are the ones that mean something to a message pushed from outside. The
// internal ones — the WiFi arcs, the play hint — are parts of screens rather
// than vocabulary.
const Icon* icon_by_name(const char* name) {
  if (!name || !name[0]) return nullptr;
  struct Named { const char* name; const Icon* icon; };
  static const Named kNamed[] = {
      {"free", &kIconFree},       {"busy", &kIconBusy},
      {"dnd", &kIconDnd},         {"moon", &kIconMoon},
      {"sun", &kIconSunCore},     {"timer", &kIconHourglass},
      {"gear", &kIconGear},       {"grid", &kIconGrid},
      {"display", &kIconDisplay}, {"hand", &kIconHand},
      {"motion", &kIconMotion},   {"lock", &kIconLock},
      {"cross", &kIconCross},     {"warning", &kIconWarning},
      {"download", &kIconDownload}, {"info", &kIconInfo},
      {"palette", &kIconPalette},
  };
  for (const Named& n : kNamed) {
    const char* a = n.name;
    const char* b = name;
    while (*a && *b) {
      // Case-folded: a host should not have to know how this table is spelled.
      const char lb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
      if (*a != lb) break;
      ++a;
      ++b;
    }
    if (*a == 0 && *b == 0) return n.icon;
  }
  return nullptr;
}

int scene_count() { return kSceneCount; }
Pattern scene_pattern(int index) {
  if (index < 0 || index >= kSceneCount) index = 0;
  return kScenes[index];
}
const char* scene_name(int index) {
  if (index < 0 || index >= kSceneCount) index = 0;
  return kSceneNames[index];
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

void draw_marquee(Framebuffer& fb, const Anim& a, const char* text, int x0,
                  int box_w, int y, RGB color) {
  if (!text || !text[0]) return;
  const int w = mini_measure_text(text);
  if (w <= box_w) {
    mini_draw_text_centered(fb, x0, box_w, y, text, color);
    return;
  }
  // Nine pixels a second: slow enough to read a word at a time, fast enough
  // that a long string comes round again before you have given up on it.
  const float span = static_cast<float>(w + box_w);
  const float x = static_cast<float>(box_w) - a.phase(span / 9.0f) * span;
  mini_draw_text_aa(fb, static_cast<float>(x0) + x, y, text, color, x0, x0 + box_w);
}

void draw_list_row(Framebuffer& fb, const MenuEntry& e, int idx, int count,
                   const Anim& a) {
  // The item breathes gently so a list left open does not look frozen, and the
  // one icon that means "machinery" turns while you look at it. That is the
  // difference between an icon and a picture of an icon.
  const float k = 0.86f + 0.14f * a.wave(3.2f);
  const RGB lit = e.color.scaled(static_cast<uint8_t>(k * 255.0f + 0.5f));
  if (e.spins) {
    draw_sprite_rotated(fb, sprite_of(*e.icon), kIconX + kIconW * 0.5f,
                        kIconH * 0.5f, a.phase(6.0f), lit, 1.0f, 1.0f,
                        Blend::Add, kIconX, kIconX + kIconW);
  } else {
    draw_icon(fb, kIconX, 0, *e.icon, lit);
  }
  // Grey rather than white. A label lights all three channels, and at 220 the
  // brightest entries went over the 2500 mA cap on their own — which would
  // have dimmed some rows and not others as you scrolled.
  mini_draw_text_centered(fb, kLabelX, kMiniLabelBox, 1, e.label,
                          RGB(140, 140, 140));

  // Where you are in the list, as ticks along the bottom row. At eight entries
  // and fifteen columns of label there is no room for a number, and a position
  // you can see at a glance beats one you have to read.
  const int span = kWidth - kLabelX;
  for (int i = 0; i < count && i < span; ++i) {
    const int x = kLabelX + (i * span) / count;
    fb.set(x, 7, i == idx ? lit : e.color.scaled(30));
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

      // Each status moves differently, not just differently coloured.
      //
      // Busy Bar gives every status its own background animation, and the idea
      // is right even though none of their artwork is usable here: at a glance
      // from across a room the *motion* is recognised before the word is read,
      // and a device whose whole job is being glanceable should use that.
      //
      // Written here, not taken — theirs are 72x16 and CC-BY-SA, and ours is
      // 24x8 and MIT.
      //
      // Restrained on purpose. These sit in somebody's peripheral vision for
      // hours; anything with a fast edge becomes an irritation by the third
      // hour, and the two that are *meant* to interrupt are the only two that
      // move quickly.
      float depth = 0.12f, period = 4.0f;
      switch (ui.status) {
        case Status::Call:  depth = 0.35f; period = 1.6f; break;  // insistent
        case Status::Busy:  depth = 0.18f; period = 2.8f; break;
        case Status::Dnd:   depth = 0.10f; period = 5.0f; break;  // flat, closed
        case Status::Focus: depth = 0.16f; period = 6.0f; break;  // slow, deep
        case Status::Away:  depth = 0.22f; period = 5.5f; break;  // drifting
        case Status::Lunch: depth = 0.14f; period = 3.4f; break;
        case Status::Meet:  depth = 0.20f; period = 2.2f; break;
        default:            depth = 0.12f; period = 4.0f; break;  // FREE, calm
      }
      const bool urgent = (ui.status == Status::Call);
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
      // Over the label's own rows, so it passes across the word. Over a filled
      // badge it would read as a smudge on the colour, so badges keep none.
      if (!status_uses_badge(ui.status))
        sheen(fb, a, 1, 6, kLabelX, kWidth - 1, c, 6.0f);
      break;
    }

    case Screen::Clock: {
      if (!ui.time_valid) {
        // No time source yet. Dashes in a dimmed accent, and no seconds hand,
        // because there are no seconds to show — the absence has to look
        // deliberate rather than like a clock stopped at midnight.
        mini_draw_text_centered(fb, 0, kWidth, 2, "--:--", ui.accent.scaled(115));
        break;
      }
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
      // A rest is a different colour, not a different layout. The face is the
      // thing you read across a room and it should not move about; what
      // changes is whether the room is being told to work or to stop.
      const RGB base = ui.timer_resting ? RGB(60, 180, 255) : ui.accent;
      RGB c = (left <= 60) ? RGB(255, 40, 20) : base;
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
      if (!ui.timer_running && left > 0) draw_play_hint(fb, a, base);

      // Which round of the set, as pips along the top row — the same idea as
      // the menu's position ticks, and for the same reason: at this size a
      // position you can see beats a number you have to read. Only drawn for a
      // set of more than one, because "round 1 of 1" is not information.
      //
      // Row 0 because it is the only row this screen leaves entirely free. The
      // first attempt put them down the left edge, where they sat on top of
      // the play hint — measured, after a test caught them there.
      if (ui.timer_cycles > 1) {
        const int n = ui.timer_cycles > 8 ? 8 : ui.timer_cycles;
        for (int i = 0; i < n; ++i) {
          const bool past = (i + 1) < ui.timer_cycle;
          const bool now_round = (i + 1) == ui.timer_cycle;
          const RGB pip = now_round ? base : (past ? base.scaled(90) : base.scaled(25));
          fb.set(i * 2, 0, pip);
        }
      }
      break;
    }

    case Screen::Menu:
    case Screen::Group: {
      // Two levels of the same list. What differs is only what is in it, which
      // the model supplies, so both draw through one function rather than
      // drifting apart as one of them is tweaked.
      const MenuEntry* items = ui.list;
      const int n = ui.list_count;
      if (!items || n <= 0) break;
      int idx = ui.menu_index;
      if (idx < 0 || idx >= n) idx = 0;
      draw_list_row(fb, items[idx], idx, n, a);
      break;
    }

    case Screen::Setting: {
      // One screen for every kind of setting, because they differ by a few
      // pixels and not by a layout: an icon that says which setting, the value
      // as text, and a rail underneath when the value is a range.
      //
      // The value arrives already rendered — "ON", "25M", "PLASMA" — so this
      // never has to know what any particular setting means. That knowledge
      // belongs with the struct the value came out of.
      const float k = 0.88f + 0.12f * a.wave(2.6f);
      const RGB lit = ui.set_tint.scaled(static_cast<uint8_t>(k * 255.0f + 0.5f));

      if (ui.set_icon) {
        // A toggle that is off is drawn dim rather than absent: the icon is
        // what tells you which setting you are looking at, and hiding it to
        // show a state would cost the identity to show the value.
        // An off toggle is dimmed, not hidden: the icon is what tells you which
        // setting you are looking at, and the word beside it already says the
        // value. 45 was too far — after gamma it rendered as black, so the
        // screen read as a missing icon rather than an inactive one.
        draw_icon(fb, kIconX, 0, *ui.set_icon,
                  ui.set_on || ui.set_fraction >= 0.0f ? lit
                                                       : ui.set_tint.scaled(110));
      }

      // The value fills the label box. Long choices scroll rather than clip,
      // for the same reason an SSID does: a name you cannot read in full is
      // not a choice you can make.
      const char* text = ui.set_text ? ui.set_text : "";
      const int y = ui.set_fraction >= 0.0f ? 0 : 1;
      draw_marquee(fb, a, text, kLabelX, kMiniLabelBox, y, RGB(170, 170, 170));

      // The rail, for anything with a range. A toggle has nowhere to be along
      // a line, so it does not get one.
      //
      // Confined to the label area rather than the full width. An icon is
      // eight rows tall and so reaches row 7 itself, and a full-width rail ran
      // straight through the bottom of it — the two were drawn over each other
      // and the rail's lit end was the part that disappeared. The menu's
      // position ticks have always started at kLabelX for the same reason.
      if (ui.set_fraction >= 0.0f) {
        const int x0 = kLabelX;
        const int span = kWidth - x0;
        const float edge = ui.set_fraction * static_cast<float>(span);
        for (int i = 0; i < span; ++i) {
          float cov = edge - static_cast<float>(i);
          if (cov < 0.0f) cov = 0.0f;
          if (cov > 1.0f) cov = 1.0f;
          fb.set(x0 + i, 7, cov > 0.5f ? lit : ui.set_tint.scaled(28));
        }
      }
      break;
    }

    case Screen::Scene: {
      // The ambient patterns, which until now were a second drawing system
      // with no way to reach them. Rendered here so a scene is a screen like
      // any other and can be transitioned into and out of.
      static Engine engine;
      static uint8_t current = 0xFF;
      const Pattern p = scene_pattern(ui.scene);
      if (current != ui.scene) {
        current = ui.scene;
        engine.set_pattern(p);
      }
      engine.render_us(fb, static_cast<micros_t>(a.t * 1e6));
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

    case Screen::WifiSetup:
    case Screen::WifiConnecting:
    case Screen::WifiInfo: {
      // Three states, one layout: a signal mark and a marquee. What differs is
      // the colour and what the arcs are doing, because those are the two
      // things readable across a room on a panel eight pixels tall.
      //
      //   setup       amber, arcs climbing slowly   — it wants something
      //   connecting  amber, arcs climbing fast     — it is working
      //   info        accent, all three arcs lit    — it is done
      const bool setup = (s == Screen::WifiSetup);
      const bool trying = (s == Screen::WifiConnecting);
      const RGB tint = (setup || trying) ? RGB(255, 150, 30) : ui.accent;

      // The arcs climb and reset, which is the most direct way a three-bar
      // signal mark can say "not yet". Faster while a join is actually in
      // flight than while waiting to be told what to join: the difference in
      // rate is what distinguishes the two amber screens at a glance.
      const float rate = trying ? 3.0f : 1.2f;
      const int lit = (setup || trying)
                          ? 1 + static_cast<int>(a.phase(1.0f / rate) * 3.0f) % 3
                          : 3;
      for (int arc = 0; arc < 3; ++arc) {
        const RGB c = (arc <= lit) ? tint : tint.scaled(28);
        // A quarter-circle of radius 2/4/6 about the bottom-left corner, which
        // in six columns is the most recognisable WiFi mark that fits.
        const int r = 2 + arc * 2;
        for (int i = 0; i <= r; ++i) {
          const float th = 1.5708f * (float)i / (float)r;
          const int x = static_cast<int>(r * std::sin(th) + 0.5f);
          const int y = 7 - static_cast<int>(r * std::cos(th) + 0.5f);
          fb.set(x, y, c);
        }
      }

      // The text scrolls because it never fits: an SSID is up to 32 characters
      // and a dotted quad is fifteen, against a panel seventeen pixels wide
      // once the mark has taken its six.
      const char* text = ui.net_text && ui.net_text[0] ? ui.net_text : "...";
      draw_marquee(fb, a, text, 8, kWidth - 8, 1, tint);
      break;
    }

    case Screen::OtaProgress: {
      // The one screen that exists to be alarming. Pulling the power part way
      // through a write is the only way to actually brick this device, so the
      // panel says so in the loudest way eight rows allow: a full-width bar
      // that fills, over a field that pulses.
      const float p = ui.ota < 0.0f ? 0.0f : (ui.ota > 1.0f ? 1.0f : ui.ota);
      const float k = 0.55f + 0.45f * a.wave(0.9f);
      const RGB warm(255, 150, 30);
      const RGB lit = warm.scaled(static_cast<uint8_t>(k * 255.0f + 0.5f));

      // Rows 0-2 carry the word, rows 4-7 the bar, so the two never collide
      // the way the setting rail and its icon did.
      mini_draw_text_centered(fb, 0, kWidth, 0, "UPDATE", RGB(150, 150, 150));
      draw_bar_aa(fb, 4, 6, p, lit, warm.scaled(24));

      // A row of ticks under the bar, so progress is readable even when the
      // bar is nearly empty and the lit part is a single dim column.
      const int lit_cols = static_cast<int>(p * kWidth + 0.5f);
      for (int x = 0; x < kWidth; x += 4)
        fb.set(x, 7, x < lit_cols ? lit : warm.scaled(20));
      break;
    }

    case Screen::Draw: {
      // One screen for everything a host can ask for, laid out by what it was
      // actually given rather than by a fixed template. Four ingredients: an
      // icon, a line of text, a countdown, a rail. Nobody sends all four —
      // there is room for about two — so the layout is decided here by which
      // ones are present.
      const float k = 0.88f + 0.12f * a.wave(2.6f);
      const RGB lit = ui.draw_tint.scaled(static_cast<uint8_t>(k * 255.0f + 0.5f));

      const bool has_icon = ui.draw_icon != nullptr;
      const bool has_time = ui.draw_seconds >= 0;
      const bool has_text = ui.draw_text && ui.draw_text[0];
      const bool has_bar = ui.draw_bar >= 0.0f;

      const int x0 = has_icon ? kLabelX : 0;
      const int box = kWidth - x0;
      if (has_icon) draw_icon(fb, kIconX, 0, *ui.draw_icon, lit);

      if (has_time && has_text) {
        // They take turns rather than sharing the panel.
        //
        // The first version stacked them — words on row 0, clock beneath — and
        // they collided: the mini font is five rows tall and so is the time
        // face, which is ten rows in a panel eight rows high. Shrinking either
        // one to fit would make both harder to read than showing one at a
        // time, and the thing a countdown is for is being readable from across
        // a room.
        //
        // Five seconds each, which is long enough to read a scrolling line and
        // short enough that the number is never far away.
        if (a.phase(10.0f) < 0.5f) {
          draw_marquee(fb, a, ui.draw_text, x0, box, 1, RGB(190, 190, 190));
        } else {
          draw_pair_face(fb, ui.draw_seconds / 60, ui.draw_seconds % 60, true, lit);
        }
      } else if (has_time) {
        draw_pair_face(fb, ui.draw_seconds / 60, ui.draw_seconds % 60, true, lit);
      } else if (has_text) {
        draw_marquee(fb, a, ui.draw_text, x0, box, has_bar ? 1 : 2, lit);
      }

      if (has_bar) draw_bar_aa(fb, 7, 7, ui.draw_bar, lit, ui.draw_tint.scaled(24));
      break;
    }

    case Screen::Paired: {
      // A list of names, one at a time, with ticks for position — the same
      // shape as the settings list, because it is the same gesture.
      if (ui.paired_count <= 0) {
        mini_draw_text_centered(fb, 0, kWidth, 1, "NONE", RGB(120, 120, 120));
        break;
      }
      int idx = ui.paired_index;
      if (idx < 0 || idx >= ui.paired_count) idx = 0;
      const RGB c = RGB(120, 200, 255);
      draw_marquee(fb, a, ui.paired[idx], 0, kWidth, 1, c);
      const int n = ui.paired_count;
      for (int i = 0; i < n && i < kWidth; ++i) {
        const int x = (i * kWidth) / n;
        fb.set(x, 7, i == idx ? c : c.scaled(30));
      }
      break;
    }

    case Screen::Confirm: {
      // What, and which way you are about to answer it.
      //
      // Two rows of text will not fit: the mini font is five tall and the
      // panel is eight, so a question above an answer overlaps by two. The
      // word says what, and a switch below it says which — a knob sliding
      // right into red reads from across the room in a way a second word
      // never would, and it is the same gesture the knob is making.
      const RGB no = RGB(150, 150, 150);
      const RGB yes = RGB(255, 60, 40);
      mini_draw_text_centered(fb, 0, kWidth, 0, ui.confirm_label,
                              ui.confirm_yes ? yes : no);
      constexpr int kKnob = 6;
      const int x = ui.confirm_yes ? kWidth - kKnob : 0;
      for (int col = 0; col < kWidth; ++col) fb.set(col, 7, RGB(40, 40, 40));
      // Breathing only on yes. At rest the thing about to be destroyed should
      // not look like it is already happening.
      const float k = ui.confirm_yes ? 0.75f + 0.25f * a.wave(1.4f) : 1.0f;
      const RGB c = (ui.confirm_yes ? yes : no)
                        .scaled(static_cast<uint8_t>(k * 255.0f + 0.5f));
      for (int i = 0; i < kKnob; ++i) {
        fb.set(x + i, 6, c);
        fb.set(x + i, 7, c.scaled(120));
      }
      break;
    }

    case Screen::Pairing: {
      // Six digits across 24 columns. The tiny face is three wide a digit plus
      // a gap, so six of them are exactly 23 — which fits, once, and is the
      // reason this screen shows a passkey rather than anything longer.
      const RGB c = RGB(120, 200, 255);
      const float k = 0.75f + 0.25f * a.wave(1.6f);
      const RGB lit = c.scaled(static_cast<uint8_t>(k * 255.0f + 0.5f));
      const int w = tiny_number_width(6);
      draw_tiny_number(fb, (kWidth - w) / 2, 1, static_cast<int>(ui.passkey % 1000000u),
                       6, lit);
      // A travelling mark underneath rather than a lit rail, so it reads as
      // something happening rather than as a number the panel has decided to
      // display.
      //
      // A full-width comet tail at 90/255 measured 2301 mA of the 2500 mA cap,
      // and a frame over the cap is scaled down — which would have dimmed the
      // digits to make room for their own decoration. Six digits are the point
      // of this screen; the mark is not.
      const float head = a.phase(2.0f) * kWidth;
      for (int k = 0; k < 4; ++k) {
        const int x = static_cast<int>(head) - k;
        if (x < 0 || x >= kWidth) continue;
        fb.set(x, 7, c.scaled(static_cast<uint8_t>(70 - k * 18)));
      }
      break;
    }

    case Screen::Booting: {
      // The disk spinning up.
      //
      // The panel is read as a radial strip of a record everywhere else, so
      // this is the same geometry put to work: a point at radius r and angle
      // theta lands at row 3.5 + r*sin(theta), which means a spoke crosses the
      // rim in a blink and the hub slowly. Looking at a turning record through
      // a narrow radial slit is exactly what that looks like, and it is the one
      // thing this panel's shape is good at.
      //
      // Three beats: a spark at the hub, the disk catching and winding up, then
      // it settles and hands over.
      const float t = ui.boot_t;

      if (t < kBootSparkS) {
        // Ignition. A point at the hub, brightening, with the first suggestion
        // of a streak leaving it.
        const float u = t / kBootSparkS;
        const float e = ease::out_cubic(u);
        const RGB c = lerp_rgb(ui.accent, RGB(255, 255, 255), 1.0f - e);
        fb.set_aa2(0.5f + e * 1.5f, 3.5f, c, 0.35f + 0.65f * e, Blend::Add);
        // A short tail, growing along the radius.
        const float reach = e * 5.0f;
        for (float d = 0.5f; d < reach; d += 1.0f) {
          const float k = 1.0f - d / 6.0f;
          fb.set_aa2(0.5f + d, 3.5f, c, k * k * 0.5f, Blend::Add);
        }
        break;
      }

      // Winding up, then settling. The spin accelerates into the middle of the
      // sequence and eases out of it, so it reads as something with mass being
      // brought up to speed rather than a loop that was switched on.
      const float u = clamp01((t - kBootSparkS) / (kBootSeconds - kBootSparkS));
      const float spin = kBootRevolutions * ease::in_out_cubic(u);
      // It fades in as it catches and out as it settles, so neither end is a cut.
      float level = 1.0f;
      if (u < 0.12f) level = u / 0.12f;
      if (u > 0.78f) level = 1.0f - (u - 0.78f) / 0.22f;
      if (level <= 0.0f) break;

      // Each spoke is drawn at several recent angles, so it smears into a
      // streak instead of strobing through a sequence of positions. At 24
      // columns a fast-moving single-pixel line reads as flicker; the trail is
      // what turns it into motion.
      for (int i = 0; i < kBootSpokes; ++i) {
        for (int tr = 0; tr < kBootTrail; ++tr) {
          const float lag = static_cast<float>(tr) * kBootTrailStep;
          const float theta = spin - lag + static_cast<float>(i) / kBootSpokes;
          const float sn = fast_sin(theta);
          const float fade = 1.0f - static_cast<float>(tr) / kBootTrail;
          const float bright = level * fade * fade;
          if (bright <= 0.01f) continue;
          for (int x = 0; x < kWidth; ++x) {
            const float r = kDiskHubRadius + static_cast<float>(x);
            const float y = 3.5f + r * sn;
            if (y < -1.0f || y > kHeight) continue;
            fb.set_aa2(static_cast<float>(x), y, ui.accent, bright, Blend::Add);
          }
        }
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
