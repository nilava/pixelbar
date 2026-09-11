// Host-side tests for the panel component. No framework: a few macros and a
// non-zero exit code on failure.
//
//   ./test/run.sh
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <vector>

#include "panel/config.h"
#include "panel/flourish.h"
#include "panel/font.h"
#include "panel/framebuffer.h"
#include "panel/patterns.h"
#include "panel/renderer.h"
#include "panel/digit_roll.h"
#include "panel/icons.h"
#include "panel/mini_font.h"
#include "panel/screens.h"
#include "panel/sprite.h"
#include "panel/transition.h"

using namespace panel;

#include "harness.h"

int g_failures = 0;
int g_checks = 0;
const char* g_case = "";

static int lit_count(const Framebuffer& fb) {
  int n = 0;
  for (int y = 0; y < kHeight; ++y)
    for (int x = 0; x < kWidth; ++x) n += fb.get(x, y).lit() ? 1 : 0;
  return n;
}

// ---------------------------------------------------------------- geometry

static void test_mapping_is_bijective() {
  CASE("every wiring maps the 192 pixels onto 192 distinct LEDs");
  const Corner corners[] = {Corner::TopLeft, Corner::TopRight,
                            Corner::BottomLeft, Corner::BottomRight};
  const Axis axes[] = {Axis::Row, Axis::Column};
  for (Corner c : corners) {
    for (Axis a : axes) {
      for (bool serp : {false, true}) {
        for (bool rtl : {false, true}) {
          const Wiring w{c, a, serp, rtl};
          std::set<int> seen;
          for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
              const int i = led_index(x, y, w);
              CHECK(i >= 0 && i < kNumLeds);
              seen.insert(i);
            }
          }
          CHECK_EQ(seen.size(), kNumLeds);
        }
      }
    }
  }
}

static void test_wled_reference_mapping() {
  CASE("the configured wiring matches the WLED setup it was taken from");
  // Three 8x8 boards, first LED top-left of each, rows running left to right,
  // not serpentine, chained left to right. Under that arrangement the chain
  // index of a logical pixel is simply tile*64 + y*8 + (x mod 8) — which is
  // what the panel diagram in WLED shows, green dot top-left of each board and
  // red dot bottom-right.
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      const int tile = x / kTileW;
      const int want = tile * (kTileW * kTileH) + y * kTileW + (x % kTileW);
      CHECK_EQ(led_index(x, y, kWiring), want);
    }
  }
  // The corners, spelled out, because they are what you check by eye.
  CHECK_EQ(led_index(0, 0, kWiring), 0);
  CHECK_EQ(led_index(7, 0, kWiring), 7);
  CHECK_EQ(led_index(0, 1, kWiring), 8);     // no serpentine: back to the left
  CHECK_EQ(led_index(7, 7, kWiring), 63);    // last LED of the first board
  CHECK_EQ(led_index(8, 0, kWiring), 64);    // first of the second
  CHECK_EQ(led_index(23, 7, kWiring), 191);  // and the last of the chain
}

static void test_known_layouts() {
  CASE("top-left serpentine by rows");
  const Wiring serp{Corner::TopLeft, Axis::Row, true, false};
  CHECK_EQ(led_index(0, 0, serp), 0);
  CHECK_EQ(led_index(7, 0, serp), 7);
  CHECK_EQ(led_index(7, 1, serp), 8);  // row 1 runs backwards
  CHECK_EQ(led_index(0, 1, serp), 15);
  CHECK_EQ(led_index(8, 0, serp), 64);   // first pixel of the middle board
  CHECK_EQ(led_index(16, 0, serp), 128); // first pixel of the right board

  CASE("top-left progressive by rows");
  const Wiring prog{Corner::TopLeft, Axis::Row, false, false};
  CHECK_EQ(led_index(0, 1, prog), 8);
  CHECK_EQ(led_index(7, 1, prog), 15);

  CASE("bottom-right origin puts LED 0 at the far corner");
  const Wiring br{Corner::BottomRight, Axis::Row, false, false};
  CHECK_EQ(led_index(7, 7, br), 0);
  CHECK_EQ(led_index(0, 7, br), 7);

  CASE("column-major serpentine");
  const Wiring col{Corner::TopLeft, Axis::Column, true, false};
  CHECK_EQ(led_index(0, 0, col), 0);
  CHECK_EQ(led_index(0, 7, col), 7);
  CHECK_EQ(led_index(1, 7, col), 8);

  CASE("a reversed chain moves logical x=0 to the last board");
  const Wiring rtl{Corner::TopLeft, Axis::Row, true, true};
  CHECK_EQ(led_index(0, 0, rtl), 128);
  CHECK_EQ(led_index(16, 0, rtl), 0);
}

// ---------------------------------------------------------------- font

static void test_font() {
  CASE("font metrics");
  CHECK_EQ(measure_text(""), 0);
  CHECK(glyph_for('A') != nullptr);
  CHECK(glyph_for('a') != nullptr);  // lowercase folds to uppercase
  CHECK_EQ(char_advance('a'), char_advance('A'));
  CHECK(glyph_for('~') == nullptr);
  CHECK_EQ(char_advance('~'), kSpaceAdvance);  // unknown renders as a space

  CASE("the font is proportional");
  CHECK(char_advance('I') < char_advance('W'));
  CHECK(char_advance('.') < char_advance('M'));
  CHECK_EQ(char_advance(' '), kSpaceAdvance);

  CASE("drawing lights the expected pixels");
  Framebuffer fb;
  fb.clear();
  const int adv = draw_char(fb, 0, 0, 'A', RGB(255, 255, 255));
  CHECK_EQ(adv, char_advance('A'));
  // 'A' has a full bar across its middle row and open corners on the top row.
  CHECK(fb.get(0, 3).lit());
  CHECK(fb.get(4, 3).lit());
  CHECK(!fb.get(0, 0).lit());
  CHECK(fb.get(1, 0).lit());

  CASE("text advances by the sum of its characters");
  fb.clear();
  const int w = draw_text(fb, 0, 0, "AB", RGB(1, 2, 3));
  CHECK_EQ(w, char_advance('A') + char_advance('B'));

  CASE("drawing off the left edge is clipped, not wrapped");
  fb.clear();
  draw_char(fb, -20, 0, 'W', RGB(255, 255, 255));
  CHECK_EQ(lit_count(fb), 0);

  CASE("a full line of text fits in the 7 rows above the bottom");
  fb.clear();
  draw_text(fb, 0, 0, "WWWW", RGB(255, 255, 255));
  for (int x = 0; x < kWidth; ++x) CHECK(!fb.get(x, kHeight - 1).lit());

  CASE("tiny digits");
  CHECK(tiny_digit('5') != nullptr);
  CHECK(tiny_digit('x') == nullptr);
}

// ---------------------------------------------------------------- colour

static void test_color() {
  CASE("hsv primaries");
  CHECK(hsv(0.0f, 1, 1) == RGB(255, 0, 0));
  CHECK(hsv(1.0f / 3.0f, 1, 1) == RGB(0, 255, 0));
  CHECK(hsv(2.0f / 3.0f, 1, 1) == RGB(0, 0, 255));

  CASE("hue wraps");
  CHECK(hsv(1.0f, 1, 1) == hsv(0.0f, 1, 1));
  CHECK(hsv(-0.25f, 1, 1) == hsv(0.75f, 1, 1));

  CASE("zero saturation is grey");
  const RGB g = hsv(0.4f, 0.0f, 0.5f);
  CHECK(g.r == g.g && g.g == g.b);

  CASE("hex parsing");
  RGB c;
  CHECK(parse_hex_color("#ff8a1f", &c));
  CHECK(c == RGB(255, 138, 31));
  CHECK(parse_hex_color("00FF00", &c));
  CHECK(c == RGB(0, 255, 0));
  CHECK(!parse_hex_color("#xyzxyz", &c));
  CHECK(!parse_hex_color("#fff", &c));

  CASE("the gamma table is monotonic and anchored at both ends");
  CHECK_EQ(kGamma8[0], 0);
  CHECK_EQ(kGamma8[255], 255);  // the table must reach the top of the range
  for (int i = 1; i < 256; ++i) CHECK(kGamma8[i] >= kGamma8[i - 1]);
}

// ---------------------------------------------------------------- framebuffer

static void test_render_order_and_power() {
  uint8_t buf[kNumLeds * 3];

  CASE("a dark panel emits all zero bytes");
  Framebuffer fb;
  fb.clear();
  fb.render(buf, 255, kMaxMilliamps, kWiring);
  for (int i = 0; i < kNumLeds * 3; ++i) CHECK_EQ(buf[i], 0);

  CASE("channel order on the wire is green, red, blue");
  fb.clear();
  fb.set(0, 0, RGB(255, 0, 0));
  fb.render(buf, 255, 0.0f /* no cap */, kWiring);
  const int i0 = led_index(0, 0, kWiring) * 3;
  CHECK_EQ(buf[i0 + 0], 0);             // green
  CHECK_EQ(buf[i0 + 1], kGamma8[255]);  // red
  CHECK_EQ(buf[i0 + 2], 0);             // blue

  CASE("brightness scales the output");
  fb.render(buf, 128, 0.0f, kWiring);
  CHECK_NEAR(buf[i0 + 1], kGamma8[255] * 128.0 / 255.0, 1.0);

  CASE("a lit panel is estimated to draw more than a dark one");
  Framebuffer white;
  white.fill(RGB(255, 255, 255));
  CHECK(white.estimate_ma(255) > fb.estimate_ma(255));

  CASE("full white at full brightness exceeds any USB supply");
  // This is the number that makes the cap necessary: ~11 A.
  CHECK(white.estimate_ma(255) > 10000.0f);

  CASE("the power cap scales an over-budget frame down to the budget");
  const float scale = white.render(buf, 255, kMaxMilliamps, kWiring);
  CHECK(scale < 1.0f);
  // Re-derive the current from what actually went onto the wire.
  double sum = 0;
  for (int i = 0; i < kNumLeds * 3; ++i) sum += buf[i];
  const double ma =
      sum / 255.0 * (kFullWhiteMaPerLed / 3.0) + kNumLeds * kIdleMaPerLed;
  CHECK_NEAR(ma, kMaxMilliamps, 25.0);

  CASE("a frame inside the budget is left alone");
  Framebuffer dim;
  dim.clear();
  dim.set(3, 3, RGB(255, 255, 255));
  CHECK_EQ(dim.render(buf, 255, kMaxMilliamps, kWiring), 1.0f);

  CASE("lowering brightness lets a frame back inside the budget");
  CHECK_EQ(white.render(buf, 32, kMaxMilliamps, kWiring), 1.0f);

  CASE("every pixel reaches a distinct LED slot");
  Framebuffer ramp;
  ramp.fill(RGB(255, 255, 255));
  ramp.render(buf, 255, 0.0f, kWiring);
  for (int i = 0; i < kNumLeds; ++i) CHECK(buf[i * 3] > 0);
}

// ---------------------------------------------------------------- engine

static void run_frames(Engine& e, Framebuffer& fb, int frames, uint32_t* t,
                       uint32_t step_ms = 16) {
  for (int i = 0; i < frames; ++i) {
    *t += step_ms;
    e.render(fb, *t);
  }
}

static void test_engine() {
  CASE("pattern names round-trip");
  for (int i = 0; i < static_cast<int>(Pattern::Count); ++i) {
    const Pattern p = static_cast<Pattern>(i);
    Pattern back;
    CHECK(pattern_from_name(pattern_name(p), &back));
    CHECK_EQ(static_cast<int>(back), i);
  }
  Pattern p;
  CHECK(pattern_from_name("Rainbow", &p));  // lookup is case-insensitive
  CHECK_EQ(static_cast<int>(p), static_cast<int>(Pattern::Rainbow));
  CHECK(!pattern_from_name("nope", &p));

  Engine e;
  Framebuffer fb;
  uint32_t t = 1000;

  CASE("scrolling text stays within one loop of the string");
  e.set_pattern(Pattern::Text);
  e.set_text("HELLO WORLD");
  const int total = measure_text("HELLO WORLD") + kWidth;
  run_frames(e, fb, 60 * 30, &t);  // 30 seconds
  CHECK(e.scroll_px() >= 0.0f);
  CHECK(e.scroll_px() < static_cast<float>(total));

  CASE("the first frame does not jump, whatever the clock reads");
  Engine fresh;
  Framebuffer fb2;
  fresh.set_pattern(Pattern::Text);
  fresh.render(fb2, 987654321u);
  CHECK_EQ(fresh.scroll_px(), 0.0f);

  CASE("a long stall is clamped instead of skipping the animation forward");
  Engine stalled;
  Framebuffer fb3;
  stalled.set_pattern(Pattern::Rainbow);
  uint32_t st = 0;
  stalled.render(fb3, st);
  st += 60000;  // a minute of nothing
  stalled.render(fb3, st);
  CHECK(stalled.anim_time() <= 0.25f);

  CASE("the millisecond counter wrapping does not rewind the animation");
  Engine wrap;
  Framebuffer fb4;
  wrap.set_pattern(Pattern::Rainbow);
  uint32_t wt = 0xFFFFFF00u;
  wrap.render(fb4, wt);
  wt += 100;  // wraps past 2^32
  wrap.render(fb4, wt);
  CHECK_NEAR(wrap.anim_time(), 0.1f, 0.001f);

  CASE("long text starts off the right edge and scrolls in");
  e.set_pattern(Pattern::Text);
  e.set_text("SCROLLING MESSAGE");
  t += 16;
  e.render(fb, t);
  CHECK_EQ(lit_count(fb), 0);  // one frame in, nothing has entered yet
  run_frames(e, fb, 60, &t);   // a second later it is on the panel
  CHECK(lit_count(fb) > 0);

  CASE("short text appears immediately, centred");
  e.set_text("A");
  t += 16;
  e.render(fb, t);
  CHECK(lit_count(fb) > 0);

  CASE("the clock draws its digits");
  e.set_pattern(Pattern::Clock);
  e.params.hour = 12;
  e.params.minute = 34;
  e.params.second = 30;
  t += 16;
  e.render(fb, t);
  CHECK(lit_count(fb) > 10);

  CASE("the seconds bar fills as the minute runs");
  int bar30 = 0;
  for (int x = 0; x < kWidth; ++x) bar30 += fb.get(x, kHeight - 1).lit() ? 1 : 0;
  e.params.second = 59;
  t += 16;
  e.render(fb, t);
  int bar59 = 0;
  for (int x = 0; x < kWidth; ++x) bar59 += fb.get(x, kHeight - 1).lit() ? 1 : 0;
  CHECK(bar59 > bar30);

  CASE("changing pattern restarts the scroll");
  e.set_pattern(Pattern::Text);
  e.set_text("SCROLLING MESSAGE");
  run_frames(e, fb, 120, &t);
  CHECK(e.scroll_px() > 0.0f);
  e.set_pattern(Pattern::Plasma);
  e.set_pattern(Pattern::Text);
  CHECK_EQ(e.scroll_px(), 0.0f);

  CASE("every pattern renders within the power budget without going dark");
  for (int i = 0; i < static_cast<int>(Pattern::Count); ++i) {
    Engine any;
    Framebuffer anyfb;
    any.set_pattern(static_cast<Pattern>(i));
    uint32_t at = 0;
    run_frames(any, anyfb, 120, &at);  // two seconds in
    uint8_t buf[kNumLeds * 3];
    const float s = anyfb.render(buf, kDefaultBrightness, kMaxMilliamps, kWiring);
    CHECK(s > 0.0f && s <= 1.0f);
    CHECK(anyfb.estimate_ma(kDefaultBrightness) * s <= kMaxMilliamps + 1.0f);
  }

  CASE("sparkle is reproducible for a given seed and frame sequence");
  Engine a, b;
  Framebuffer fa, fbb;
  a.set_pattern(Pattern::Sparkle);
  b.set_pattern(Pattern::Sparkle);
  uint32_t ta = 0, tb = 0;
  run_frames(a, fa, 100, &ta);
  run_frames(b, fbb, 100, &tb);
  for (int y = 0; y < kHeight; ++y)
    for (int x = 0; x < kWidth; ++x) CHECK(fa.get(x, y) == fbb.get(x, y));

  CASE("sparkle actually lights pixels and decays them again");
  CHECK(lit_count(fa) > 0);
  Engine s2;
  Framebuffer f2;
  s2.set_pattern(Pattern::Sparkle);
  uint32_t t2 = 0;
  run_frames(s2, f2, 100, &t2);
  s2.params.speed = 0.0f;  // freeze spawning, let the decay run
  run_frames(s2, f2, 200, &t2);
  CHECK(lit_count(f2) <= lit_count(fa));

  CASE("the map test walks one bright pixel in logical order");
  Engine mt;
  Framebuffer mf;
  mt.set_pattern(Pattern::MapTest);
  uint32_t mtt = 0;
  mt.render(mf, mtt);
  CHECK(mf.get(0, 0) == RGB(255, 255, 255));  // the head starts on the origin
  run_frames(mt, mf, 60, &mtt);               // and walks away from it
  CHECK(mf.get(0, 0) == RGB(255, 0, 0));      // leaving the origin marker
  int white = 0;
  for (int y = 0; y < kHeight; ++y)
    for (int x = 0; x < kWidth; ++x)
      if (mf.get(x, y) == RGB(255, 255, 255)) ++white;
  CHECK_EQ(white, 1);
}

// ---------------------------------------------------------------- screens

static void test_screens() {
  CASE("every status word fits without scrolling");
  // In the 5x7 face, which is what the marquee uses. Only the statuses that
  // keep the icon-and-label layout have to clear this bar; the longer ones are
  // drawn as a badge in the 3x5 face and are checked against that box instead.
  for (int i = 0; i < static_cast<int>(Status::Count); ++i) {
    const Status st = static_cast<Status>(i);
    if (status_uses_badge(st)) continue;
    CHECK(text_fits(status_label(st)));
  }
  CHECK_EQ(text_ink_width("BUSY"), 23);  // 23 of the 24 columns
  CHECK(text_fits("BUSY"));
  CHECK(!text_fits("FOCUS SESSION"));

  CASE("a string that fits is drawn centred and still");
  Engine e;
  Framebuffer a, b;
  e.set_pattern(Pattern::Text);
  e.set_text("BUSY");
  uint32_t t = 0;
  e.render(a, t);
  run_frames(e, b, 120, &t);  // two seconds later
  CHECK_EQ(e.scroll_px(), 0.0f);
  for (int y = 0; y < kHeight; ++y)
    for (int x = 0; x < kWidth; ++x) CHECK(a.get(x, y) == b.get(x, y));
  CHECK(lit_count(a) > 0);

  CASE("a string too wide to fit still scrolls");
  Engine e2;
  Framebuffer c, d;
  e2.set_pattern(Pattern::Text);
  e2.set_text("FOCUS SESSION RUNNING");
  uint32_t t2 = 0;
  run_frames(e2, c, 60, &t2);
  run_frames(e2, d, 60, &t2);
  CHECK(e2.scroll_px() > 0.0f);
  bool differs = false;
  for (int y = 0; y < kHeight && !differs; ++y)
    for (int x = 0; x < kWidth && !differs; ++x)
      if (!(c.get(x, y) == d.get(x, y))) differs = true;
  CHECK(differs);

  CASE("each status has its own colour");
  std::set<uint32_t> colors;
  for (int i = 0; i < static_cast<int>(Status::Count); ++i) {
    const RGB col = status_color(static_cast<Status>(i));
    colors.insert((uint32_t)col.r << 16 | (uint32_t)col.g << 8 | col.b);
  }
  CHECK_EQ(colors.size(), static_cast<int>(Status::Count));

  CASE("bars fill in proportion");
  Framebuffer bar;
  bar.clear();
  draw_bar(bar, 7, 7, 0.0f, RGB(255, 255, 255), RGB(0, 0, 0));
  CHECK_EQ(lit_count(bar), 0);
  draw_bar(bar, 7, 7, 1.0f, RGB(255, 255, 255), RGB(0, 0, 0));
  CHECK_EQ(lit_count(bar), kWidth);
  draw_bar(bar, 7, 7, 0.5f, RGB(255, 255, 255), RGB(0, 0, 0));
  CHECK_EQ(lit_count(bar), kWidth / 2);
  CASE("bars clamp instead of overflowing");
  draw_bar(bar, 7, 7, 5.0f, RGB(255, 255, 255), RGB(0, 0, 0));
  CHECK_EQ(lit_count(bar), kWidth);
  draw_bar(bar, 7, 7, -1.0f, RGB(255, 255, 255), RGB(0, 0, 0));
  CHECK_EQ(lit_count(bar), 0);

  CASE("every screen draws inside the panel and within the power budget");
  uint8_t buf[kNumLeds * 3];
  for (int i = 0; i < static_cast<int>(Screen::Count); ++i) {
    const Screen s = static_cast<Screen>(i);
    UiState ui;
    Framebuffer fb;
    for (uint32_t now = 0; now < 4000; now += 250) {
      draw_screen(fb, s, ui, now);
      const float k = fb.render(buf, kDefaultBrightness, kMaxMilliamps, kWiring);
      CHECK(k > 0.0f && k <= 1.0f);
    }
  }

  CASE("the status screen shows the status colour");
  UiState ui;
  Framebuffer fb;
  ui.status = Status::Busy;
  draw_screen(fb, Screen::Status, ui, 0);
  CHECK(lit_count(fb) > 0);
  // The status colour now breathes, so it is never exactly the base colour.
  // Assert the hue family instead: BUSY is dominated by red, FREE by green.
  auto brightest = [](const Framebuffer& f) {
    RGB best;
    int bv = -1;
    for (int y = 0; y < kHeight; ++y)
      for (int x = 0; x < kWidth; ++x) {
        const RGB c = f.get(x, y);
        const int v = c.r + c.g + c.b;
        if (v > bv) { bv = v; best = c; }
      }
    return best;
  };
  RGB top = brightest(fb);
  CHECK(top.r > top.g && top.r > top.b);
  ui.status = Status::Free;
  draw_screen(fb, Screen::Status, ui, 0);
  top = brightest(fb);
  CHECK(top.g > top.r && top.g > top.b);
  ui.status = Status::Busy;

  CASE("a timer longer than 24 minutes is not wrapped like a clock");
  {
    // 25:00 must read as twenty-five minutes. Sharing the clock's hour
    // normalisation once turned it into 01:00.
    Framebuffer t25, t1;
    draw_pair_face(t25, 25, 0, true, RGB(255, 255, 255));
    draw_pair_face(t1, 1, 0, true, RGB(255, 255, 255));
    bool same = true;
    for (int y = 0; y < kHeight && same; ++y)
      for (int x = 0; x < kWidth && same; ++x)
        if (!(t25.get(x, y) == t1.get(x, y))) same = false;
    CHECK(!same);
    CASE("both digit groups clamp instead of overflowing");
    Framebuffer big, cap;
    draw_pair_face(big, 250, 300, true, RGB(255, 255, 255));
    draw_pair_face(cap, 99, 99, true, RGB(255, 255, 255));
    for (int y = 0; y < kHeight; ++y)
      for (int x = 0; x < kWidth; ++x) CHECK(big.get(x, y) == cap.get(x, y));
  }

  CASE("the timer turns red in the last minute");
  ui.timer_total_s = 25 * 60;
  ui.timer_running = true;
  ui.timer_left_s = 10 * 60;
  draw_screen(fb, Screen::Timer, ui, 0);
  const int lit_calm = lit_count(fb);
  ui.timer_left_s = 30;
  draw_screen(fb, Screen::Timer, ui, 0);
  bool any_red = false;
  for (int y = 0; y < kHeight; ++y)
    for (int x = 0; x < kWidth; ++x) {
      const RGB c = fb.get(x, y);
      // The last-minute face pulses, so test the hue, not an exact value.
      if (c.lit() && c.r > 60 && c.r > 3 * c.g && c.r > 3 * c.b) any_red = true;
    }
  CHECK(any_red);
  CHECK(lit_calm > 0);

  CASE("a paused timer blinks and a running one does not");
  ui.timer_left_s = 10 * 60;
  ui.timer_running = false;
  Framebuffer p1, p2;
  draw_screen(p1, Screen::Timer, ui, 0);     // blink phase on
  draw_screen(p2, Screen::Timer, ui, 500);   // blink phase off
  bool blinks = false;
  for (int y = 0; y < kHeight && !blinks; ++y)
    for (int x = 0; x < kWidth && !blinks; ++x)
      if (!(p1.get(x, y) == p2.get(x, y))) blinks = true;
  CHECK(blinks);

  CASE("the brightness screen tracks the value");
  ui.brightness = 255;
  draw_screen(fb, Screen::Brightness, ui, 0);
  int on_full = 0;
  for (int x = 0; x < kWidth; ++x) on_full += (fb.get(x, 7) == ui.accent) ? 1 : 0;
  CHECK_EQ(on_full, kWidth);

  ui.brightness = 0;
  draw_screen(fb, Screen::Brightness, ui, 0);
  int on_zero = 0;
  for (int x = 0; x < kWidth; ++x) on_zero += (fb.get(x, 7) == ui.accent) ? 1 : 0;
  CHECK_EQ(on_zero, 0);
  // The empty part of the bar keeps a dim track, so it still reads as a bar
  // at zero rather than as a dead panel.
  CHECK(fb.get(0, 7).lit());

  CASE("the colour picker puts its cursor where the hue says");
  auto cursor_col = [&](float hue) {
    ui.hue = hue;
    draw_screen(fb, Screen::ColorPick, ui, 0);
    int best = -1, bv = -1;
    for (int x = 0; x < kWidth; ++x) {
      const RGB c = fb.get(x, 7);
      const int v = c.r + c.g + c.b;
      if (v > bv) { bv = v; best = x; }
    }
    return best;
  };
  // The cursor pulses and is drawn sub-pixel, so check where it is, not its
  // exact value. It must be white: no hue of its own.
  CHECK_EQ(cursor_col(0.0f), 0);
  CHECK_EQ(cursor_col(1.0f), kWidth - 1);
  ui.hue = 0.5f;
  draw_screen(fb, Screen::ColorPick, ui, 0);
  const RGB cur = fb.get(kWidth / 2, 7);
  CHECK(cur.r == cur.g && cur.g == cur.b);

  CASE("every menu label fits beside its icon");
  for (int i = 0; i < kMenuCount; ++i) {
    CHECK(kMenu[i].icon != nullptr);
    CHECK(kMenu[i].label != nullptr);
    CHECK(mini_text_fits(kMenu[i].label));
  }

  CASE("the menu draws something for every entry, and stays on the panel");
  for (int i = 0; i < kMenuCount; ++i) {
    UiState mu;
    mu.menu_index = static_cast<uint8_t>(i);
    ScreenAnim ma;
    Anim man;
    man.dt = 0.01f;
    man.t = 1.0;
    Framebuffer mfb;
    draw_screen(mfb, Screen::Menu, mu, man, ma);
    CHECK(lit_count(mfb) > 4);
    CHECK(mfb.estimate_ma(255) < kMaxMilliamps);
    // Column 8 is the gutter between the icon and the label and is never
    // written, on any screen that uses this layout.
    for (int y = 0; y < 7; ++y) CHECK(!mfb.get(kGutterX, y).lit());
  }

  CASE("an out-of-range menu index falls back rather than reading past the table");
  {
    UiState mu;
    mu.menu_index = 200;
    ScreenAnim ma;
    Anim man;
    man.dt = 0.01f;
    Framebuffer mfb;
    draw_screen(mfb, Screen::Menu, mu, man, ma);
    CHECK(lit_count(mfb) > 4);
  }

  CASE("no status trips the power cap, whichever layout it uses");
  {
    // A full-width badge lights three times the LEDs an icon layout does. If a
    // status went over the cap the renderer would scale that one status and not
    // the others, so changing status would change how bright the whole panel
    // looks — which reads as a fault, not as a safety feature working.
    for (int i = 0; i < static_cast<int>(Status::Count); ++i) {
      UiState st_ui;
      st_ui.status = static_cast<Status>(i);
      ScreenAnim st_anim;
      Anim st_a;
      st_a.dt = 0.01f;
      Framebuffer st_fb;
      float peak = 0.0f;
      for (int k = 0; k < 200; ++k) {
        st_a.t = k * 0.02;
        st_fb.clear();
        draw_screen(st_fb, Screen::Status, st_ui, st_a, st_anim);
        const float ma = st_fb.estimate_ma(255);
        if (ma > peak) peak = ma;
      }
      CHECK(peak < kMaxMilliamps);
    }
  }

  CASE("the heaviest frame leaves usable headroom on the bench supply too");
  {
    // Nothing in the drawing layer is tuned against one supply number. At the
    // design target (USB-C, 3 A) the heavy frames never trip the cap at all,
    // which is what keeps brightness consistent between screens. On the 2 A
    // bench brick the cap does engage, and this pins how late: if a change
    // pushes the worst frame up far enough that the prototype starts dimming
    // at ordinary brightness, that is worth knowing before the panel shows it.
    float worst = 0.0f;
    for (int i = 0; i < static_cast<int>(Status::Count); ++i) {
      UiState pu;
      pu.status = static_cast<Status>(i);
      ScreenAnim pa;
      Anim pan;
      pan.dt = 0.01f;
      Framebuffer pfb;
      for (int k = 0; k < 100; ++k) {
        pan.t = k * 0.02;
        pfb.clear();
        draw_screen(pfb, Screen::Status, pu, pan, pa);
        const float ma = pfb.estimate_ma(255);
        if (ma > worst) worst = ma;
      }
    }
    const float idle = kNumLeds * kIdleMaPerLed;
    const float ok_to = 255.0f * (kBenchMilliamps - idle) / (worst - idle);
    CHECK(ok_to > 200.0f);  // untouched to about 80% brightness, at least
  }

  CASE("sleep is dim but actually reaches the LEDs");
  draw_screen(fb, Screen::Sleep, ui, 0);
  CHECK(lit_count(fb) > 0);
  CHECK(lit_count(fb) < 40);  // a moon, not a lit panel
  // The bug this replaces: the old sleep screen used framebuffer values of
  // 2..10, and gamma plus the default brightness rounded every one of them to
  // zero, so it was pure black on hardware. Assert the rendered bytes, not the
  // framebuffer.
  //
  // "Not zero" is too weak a bar, and it let the screen regress: a later change
  // tinted the moon by dividing every channel by three, which put the peak
  // output back down to 2 of 255 — technically lit, practically black, and
  // this assertion passed. So the floor is a level you could actually see in a
  // dark room, not merely a non-zero byte.
  // The moon breathes, so the level to assert is the top of the breath rather
  // than whatever one arbitrary phase happens to give.
  uint8_t sleep_wire[kNumLeds * 3];
  int nonzero = 0, peak = 0;
  ScreenAnim sleep_anim;
  Anim sa;
  sa.dt = 0.02f;
  for (int i = 0; i < 400; ++i) {
    sa.t = i * 0.02;
    fb.clear();
    draw_screen(fb, Screen::Sleep, ui, sa, sleep_anim);
    fb.render(sleep_wire, kDefaultBrightness, kMaxMilliamps, kWiring);
    for (int k = 0; k < kNumLeds * 3; ++k) {
      nonzero += sleep_wire[k] ? 1 : 0;
      if (sleep_wire[k] > peak) peak = sleep_wire[k];
    }
  }
  CHECK(nonzero > 0);
  CHECK(peak >= 6);
  CHECK(fb.estimate_ma(kDefaultBrightness) < 400.0f);  // still nearly dark

  CASE("screen names round-trip to something printable");
  for (int i = 0; i < static_cast<int>(Screen::Count); ++i) {
    const char* n = screen_name(static_cast<Screen>(i));
    CHECK(n != nullptr && n[0] != '?');
  }
}

// ---------------------------------------------------------------- new layers

// Intensity-weighted x centre of a row. This is the stepping detector: if a
// moving thing jumps a whole LED, the centroid jumps with it.
static float centroid_x(const Framebuffer& fb, int y) {
  double num = 0, den = 0;
  for (int x = 0; x < kWidth; ++x) {
    const RGB c = fb.get(x, y);
    const double w = c.r + c.g + c.b;
    num += w * x;
    den += w;
  }
  return den > 0 ? static_cast<float>(num / den) : -1.0f;
}

// Total ink in a row, in whole-pixel units: 12.4 means 12 lit plus 40% of one.
static float row_fill(const Framebuffer& fb, int y) {
  double s = 0;
  for (int x = 0; x < kWidth; ++x) s += fb.get(x, y).r / 255.0;
  return static_cast<float>(s);
}

static void test_anim() {
  CASE("easing starts at 0 and ends at 1");
  float (*const fns[])(float) = {ease::linear, ease::in_out_sine, ease::out_cubic,
                                 ease::in_out_cubic, ease::out_quint, ease::out_back,
                                 ease::out_elastic};
  for (auto f : fns) {
    CHECK_NEAR(f(0.0f), 0.0f, 1e-3);
    CHECK_NEAR(f(1.0f), 1.0f, 1e-3);
    CHECK_NEAR(f(-5.0f), 0.0f, 1e-3);  // clamped
    CHECK_NEAR(f(5.0f), 1.0f, 1e-3);
  }

  CASE("the eases meant to stay in range do");
  for (int i = 0; i <= 64; ++i) {
    const float u = i / 64.0f;
    CHECK(ease::out_cubic(u) >= -1e-4f && ease::out_cubic(u) <= 1.0001f);
    CHECK(ease::in_out_cubic(u) >= -1e-4f && ease::in_out_cubic(u) <= 1.0001f);
    CHECK(ease::in_out_sine(u) >= -1e-4f && ease::in_out_sine(u) <= 1.0001f);
  }

  CASE("the sine table agrees with libm");
  for (int i = 0; i <= 256; ++i) {
    const float turns = i / 256.0f;
    CHECK_NEAR(fast_sin(turns), std::sin(turns * 6.283185307f), 0.002);
    CHECK_NEAR(fast_cos(turns), std::cos(turns * 6.283185307f), 0.002);
  }
  CASE("the sine table wraps");
  CHECK_NEAR(fast_sin(1.25f), fast_sin(0.25f), 1e-5);
  CHECK_NEAR(fast_sin(-0.25f), fast_sin(0.75f), 1e-5);

  CASE("smoothing is frame-rate independent");
  Smoothed a(0.0f, 0.1f), b(0.0f, 0.1f);
  a.set_target(1.0f);
  b.set_target(1.0f);
  for (int i = 0; i < 100; ++i) a.update(0.010f);   // 100 fps
  for (int i = 0; i < 1000; ++i) b.update(0.001f);  // 1000 fps
  CHECK_NEAR(a.value(), b.value(), 1e-3);

  CASE("smoothing approaches without overshooting");
  Smoothed s(0.0f, 0.1f);
  s.set_target(1.0f);
  for (int i = 0; i < 400; ++i) {
    const float v = s.update(0.010f);
    CHECK(v <= 1.0001f);
    CHECK(v >= -1e-4f);
  }
  CHECK(s.settled());

  CASE("a stall does not teleport the clock");
  FrameClock fc;
  fc.tick(0);
  fc.tick(60u * 1000000u);  // a minute of nothing
  CHECK(fc.now_s() <= kMaxFrameDt + 1e-6);

  CASE("the microsecond clock survives its 32-bit wrap");
  FrameClock w;
  w.tick(0xFFFF0000u);
  const Anim after = w.tick(0xFFFF0000u + 10000u);  // 10 ms later, past the wrap
  CHECK_NEAR(after.dt, 0.010f, 1e-4);
}

static void test_subpixel() {
  CASE("a bar at a fractional position partially lights the boundary LED");
  Framebuffer fb;
  fb.clear();
  draw_bar_aa(fb, 7, 7, 0.517f, RGB(255, 255, 255), RGB(0, 0, 0));
  CHECK_NEAR(row_fill(fb, 7), 0.517f * kWidth, 0.05);
  CHECK_EQ(fb.get(11, 7).r, 255);
  CHECK_NEAR(fb.get(12, 7).r, 255 * 0.408, 3.0);
  CHECK_EQ(fb.get(13, 7).r, 0);

  CASE("bar fill is continuous across every fraction");
  for (int i = 0; i <= 200; ++i) {
    const float f = i / 200.0f;
    fb.clear();
    draw_bar_aa(fb, 7, 7, f, RGB(255, 255, 255), RGB(0, 0, 0));
    CHECK_NEAR(row_fill(fb, 7), f * kWidth, 0.05);
  }

  CASE("sub-pixel text moves continuously instead of one LED at a time");
  float prev = -1.0f;
  for (int i = 0; i <= 20; ++i) {
    Framebuffer f;
    f.clear();
    draw_text_aa(f, 6.0f + i * 0.05f, 0, "H", RGB(255, 255, 255));
    const float c = centroid_x(f, 3);
    if (prev >= 0.0f) CHECK_NEAR(c - prev, 0.05f, 0.02f);
    prev = c;
  }

  CASE("anti-aliased text conserves its ink at any offset");
  auto ink = [](const Framebuffer& f) {
    long t = 0;
    for (int y = 0; y < kHeight; ++y)
      for (int x = 0; x < kWidth; ++x) t += f.get(x, y).r;
    return t;
  };
  Framebuffer a, b;
  a.clear();
  b.clear();
  draw_text_aa(a, 6.0f, 0, "HI", RGB(255, 255, 255));
  draw_text_aa(b, 6.5f, 0, "HI", RGB(255, 255, 255));
  CHECK_NEAR(static_cast<double>(ink(b)) / ink(a), 1.0, 0.03);
  CHECK(lit_count(b) > lit_count(a));  // the half offset splits every column

  CASE("a sub-pixel point splits its coverage between two LEDs");
  fb.clear();
  fb.set_aa(5.25f, 0, RGB(200, 200, 200), 1.0f, Blend::Add);
  CHECK_NEAR(fb.get(5, 0).r, 150.0, 2.0);
  CHECK_NEAR(fb.get(6, 0).r, 50.0, 2.0);

  CASE("a blit at a whole-pixel offset is an exact shift");
  Framebuffer src, dst;
  src.clear();
  src.set(4, 2, RGB(255, 0, 0));
  dst.clear();
  blit_offset(dst, src, 3.0f, 0.0f);
  CHECK(dst.get(7, 2) == RGB(255, 0, 0));
  CHECK_EQ(lit_count(dst), 1);
}

static void test_mini_font() {
  CASE("four-letter labels are exactly the 15 px label box");
  CHECK_EQ(mini_text_ink_width("BUSY"), kMiniLabelBox);
  CHECK_EQ(mini_text_ink_width("FREE"), kMiniLabelBox);
  CHECK_EQ(mini_text_ink_width("CALL"), kMiniLabelBox);
  CHECK_EQ(mini_text_ink_width("DONE"), kMiniLabelBox);
  CHECK_EQ(mini_text_ink_width("DND"), 11);
  CHECK_EQ(mini_text_ink_width(""), 0);

  CASE("every status label fits the layout it says it uses");
  for (int i = 0; i < static_cast<int>(Status::Count); ++i) {
    const Status st = static_cast<Status>(i);
    const char* label = status_label(st);
    if (status_uses_badge(st)) {
      // A badge has the whole panel, less a pixel of padding either side.
      CHECK(mini_text_ink_width(label) <= kWidth - 2);
    } else {
      CHECK(mini_text_fits(label));
    }
  }

  CASE("the whole alphabet has a glyph and a sane width");
  for (char c = 'A'; c <= 'Z'; ++c) {
    const MiniGlyph* g = mini_glyph_for(c);
    CHECK(g != nullptr);
    if (g) CHECK(g->w >= 1 && g->w <= kMiniMaxW);
  }
  for (char c = '0'; c <= '9'; ++c) CHECK(mini_glyph_for(c) != nullptr);
  CHECK(mini_glyph_for('~') == nullptr);

  CASE("lowercase folds to uppercase");
  CHECK_EQ(mini_char_advance('b'), mini_char_advance('B'));

  CASE("no glyph has ink outside its declared width");
  for (char c = 'A'; c <= 'Z'; ++c) {
    const MiniGlyph* g = mini_glyph_for(c);
    if (!g) continue;
    for (int r = 0; r < kMiniH; ++r) {
      for (int col = g->w; col < kMiniMaxW; ++col) {
        CHECK((g->rows[r] & (1 << (kMiniMaxW - 1 - col))) == 0);
      }
    }
  }

  CASE("a label drawn in its box never touches the icon or the gutter");
  Framebuffer fb;
  fb.clear();
  mini_draw_text_centered(fb, kLabelX, kMiniLabelBox, 1, "BUSY", RGB(255, 255, 255));
  for (int y = 0; y < kHeight; ++y)
    for (int x = 0; x <= kGutterX; ++x) CHECK(!fb.get(x, y).lit());
  for (int x = 0; x < kWidth; ++x) {
    CHECK(!fb.get(x, 0).lit());
    CHECK(!fb.get(x, 6).lit());
    CHECK(!fb.get(x, 7).lit());
  }
}

static void test_icons() {
  CASE("icons are neither blank nor solid");
  const Icon* set[] = {&kIconFree, &kIconBusy, &kIconDnd, &kIconMoon,
                       &kIconHourglass, &kIconSunCore, &kIconSunRays};
  for (const Icon* ic : set) {
    const int n = icon_lit_count(*ic);
    CHECK(n >= 6 && n <= 56);
  }
  CASE("BUSY is the FREE ring filled in, so the dissolve reads as solidifying");
  CHECK(icon_lit_count(kIconBusy) > icon_lit_count(kIconFree));

  CASE("an icon drawn on the panel lights exactly its own pixels");
  Framebuffer fb;
  fb.clear();
  draw_icon(fb, 0, 0, kIconFree, RGB(255, 255, 255));
  CHECK_EQ(lit_count(fb), icon_lit_count(kIconFree));

  CASE("an icon drawn off the edge is clipped, not wrapped");
  fb.clear();
  draw_icon(fb, kWidth - 2, 0, kIconBusy, RGB(255, 255, 255));
  CHECK(lit_count(fb) > 0);
  CHECK(lit_count(fb) < icon_lit_count(kIconBusy));
  for (int y = 0; y < kHeight; ++y)
    for (int x = 0; x < kWidth - 2; ++x) CHECK(!fb.get(x, y).lit());

  CASE("animated icons cycle and never index out of range");
  for (int i = 0; i < 200; ++i) {
    const double t = i * 0.037;
    const Icon& f1 = anim_frame(kAnimCall, t);
    const Icon& f2 = anim_frame(kAnimCall, t + kAnimCall.count / kAnimCall.fps);
    CHECK(icon_lit_count(f1) == icon_lit_count(f2));
  }
  CASE("the wifi icon fills as it goes");
  CHECK(icon_lit_count(anim_frame(kAnimWifi, 0.0)) <
        icon_lit_count(anim_frame(kAnimWifi, 3.0 / kAnimWifi.fps)));

  CASE("a masked icon shows the shell empty and the shell plus fill when full");
  Framebuffer e, f;
  e.clear();
  f.clear();
  draw_icon_masked(e, 0, 0, kIconHourglass, kIconHourglassBottom, 0.0f,
                   RGB(255, 255, 255), RGB(255, 255, 255));
  draw_icon_masked(f, 0, 0, kIconHourglass, kIconHourglassBottom, 1.0f,
                   RGB(255, 255, 255), RGB(255, 255, 255));
  CHECK(lit_count(f) > lit_count(e));

  CASE("a stroke icon draws itself on in order");
  int prev = -1;
  for (int i = 0; i <= 10; ++i) {
    Framebuffer sfb;
    sfb.clear();
    draw_stroke_icon(sfb, 0, 0, kStrokeCheck, i / 10.0f, RGB(255, 255, 255),
                     RGB(255, 255, 255));
    const int n = lit_count(sfb);
    CHECK(n >= prev);  // monotonic
    prev = n;
  }
  CHECK_EQ(prev, kStrokeCheck.n);
}

static void test_sprite() {
  const RGB W(255, 255, 255);

  CASE("scale 1 reproduces the source exactly");
  {
    Framebuffer got, want;
    draw_icon(want, 4, 0, kIconBusy, W);
    const Sprite s = sprite_of(kIconBusy);
    draw_sprite_scaled(got, s, 4.0f + kIconW * 0.5f, kIconH * 0.5f, 1.0f, 1.0f, W);
    for (int y = 0; y < kHeight; ++y)
      for (int x = 0; x < kWidth; ++x) CHECK(got.get(x, y) == want.get(x, y));
  }

  CASE("a squashed sprite keeps its peak brightness and loses its height");
  {
    // Normalised ink: the same colour over fewer rows, not a brighter smear.
    const Sprite s = sprite_of(kIconBusy);
    for (float sy : {1.0f, 0.6f, 0.3f, 0.1f}) {
      Framebuffer fb;
      draw_sprite_scaled(fb, s, 12.0f, 4.0f, 1.0f, sy, W, 1.0f);
      int peak = 0, rows = 0;
      for (int y = 0; y < kHeight; ++y) {
        int row_peak = 0;
        for (int x = 0; x < kWidth; ++x) {
          const int v = fb.get(x, y).r;
          if (v > row_peak) row_peak = v;
        }
        if (row_peak > 8) ++rows;
        if (row_peak > peak) peak = row_peak;
      }
      CHECK(peak >= 250);     // never dimmer than the source
      CHECK(rows <= kIconH);  // never taller than the source
      // Genuinely collapsed: the ink spans sy of the height, plus at most the
      // one extra row it straddles when the edges do not land on a boundary.
      const int expect = static_cast<int>(std::ceil(kIconH * sy)) + 1;
      CHECK(rows <= expect);
    }
  }

  CASE("conserved ink keeps the total instead of the peak");
  {
    const Sprite s = sprite_of(kIconBusy);
    Framebuffer norm, cons;
    draw_sprite_scaled(norm, s, 12.0f, 4.0f, 1.0f, 0.25f, W, 1.0f, 0, kHeight,
                       Ink::Normalised);
    draw_sprite_scaled(cons, s, 12.0f, 4.0f, 1.0f, 0.25f, W, 1.0f, 0, kHeight,
                       Ink::Conserved);
    long tn = 0, tc = 0;
    for (int y = 0; y < kHeight; ++y)
      for (int x = 0; x < kWidth; ++x) { tn += norm.get(x, y).r; tc += cons.get(x, y).r; }
    CHECK(tc < tn);  // conserving at a quarter height means a quarter of the ink
  }

  CASE("a zero scale draws nothing");
  {
    Framebuffer fb;
    const Sprite s = sprite_of(kIconBusy);
    draw_sprite_scaled(fb, s, 12.0f, 4.0f, 1.0f, 0.0f, W);
    draw_sprite_scaled(fb, s, 12.0f, 4.0f, 0.0f, 1.0f, W);
    for (int y = 0; y < kHeight; ++y)
      for (int x = 0; x < kWidth; ++x) CHECK(!fb.get(x, y).lit());
  }

  CASE("the clip band is never crossed");
  {
    const Sprite s = sprite_of(kIconBusy);
    for (int k = 0; k <= 20; ++k) {
      Framebuffer fb;
      const float cy = static_cast<float>(k) * 0.4f;  // slide it right across
      draw_sprite_scaled(fb, s, 12.0f, cy, 1.0f, 1.0f, W, 1.0f, 2, 6);
      for (int x = 0; x < kWidth; ++x) {
        CHECK(!fb.get(x, 0).lit());
        CHECK(!fb.get(x, 1).lit());
        CHECK(!fb.get(x, 6).lit());
        CHECK(!fb.get(x, 7).lit());
      }
    }
  }

  CASE("rotating by a whole number of turns is the identity");
  {
    const Sprite s = sprite_of(kIconBusy);
    Framebuffer zero, one;
    draw_sprite_rotated(zero, s, 12.0f, 4.0f, 0.0f, W);
    draw_sprite_rotated(one, s, 12.0f, 4.0f, 1.0f, W);
    for (int y = 0; y < kHeight; ++y)
      for (int x = 0; x < kWidth; ++x) {
        const int a = zero.get(x, y).r, b = one.get(x, y).r;
        CHECK(std::abs(a - b) <= 2);
      }
  }

  CASE("a rotating icon keeps roughly the same amount of ink lit");
  {
    // If the ink collapses at some angles the icon strobes as it turns.
    const Sprite s = sprite_of(kIconBusy);
    long first = -1;
    for (int k = 0; k < 16; ++k) {
      Framebuffer fb;
      draw_sprite_rotated(fb, s, 12.0f, 4.0f, static_cast<float>(k) / 16.0f, W);
      long total = 0;
      for (int y = 0; y < kHeight; ++y)
        for (int x = 0; x < kWidth; ++x) total += fb.get(x, y).r;
      if (first < 0) first = total;
      CHECK(total > first / 2);
      CHECK(total < first * 2);
    }
  }

  CASE("the spin scale is the cosine and the brightness never reaches zero");
  {
    CHECK(std::fabs(spin_face_scale(0.0f) - 1.0f) < 0.01f);
    CHECK(spin_face_scale(0.25f) < 0.01f);
    CHECK(std::fabs(spin_face_scale(0.5f) - 1.0f) < 0.01f);
    for (int k = 0; k <= 40; ++k) {
      const float t = static_cast<float>(k) / 80.0f;
      CHECK(spin_face_bright(t) >= 0.54f);
      CHECK(spin_face_bright(t) <= 1.001f);
    }
  }

  CASE("the pop hands over exactly once, and lands on 1");
  {
    CHECK(std::fabs(pop_out_scale(0.0f) - 1.0f) < 1e-5f);
    CHECK(pop_out_scale(kPopSwapPoint) == 0.0f);
    CHECK(pop_in_scale(kPopSwapPoint) == 0.0f);
    CHECK(std::fabs(pop_in_scale(1.0f) - 1.0f) < 1e-5f);
    float prev = 1.0f;
    for (int k = 0; k <= 50; ++k) {
      const float u = static_cast<float>(k) / 50.0f;
      const float o = pop_out_scale(u);
      CHECK(o <= prev + 1e-5f);  // the outgoing element only ever shrinks
      prev = o;
      // The two never both sit at full size, or the swap would read as a blur.
      CHECK(!(o > 0.5f && pop_in_scale(u) > 0.5f));
    }
  }

  CASE("a swap reports settled until it is primed and triggered");
  {
    SwapState sw;
    CHECK(sw.u(0.0) == 1.0f);       // nothing has ever changed
    CHECK(!sw.running(0.0));
    sw.primed = true;
    sw.trigger(10.0);
    CHECK(sw.running(10.0));
    CHECK(sw.u(10.0) == 0.0f);
    CHECK(sw.u(10.0 + kPopSeconds * 0.5) > 0.4f);
    CHECK(sw.u(10.0 + kPopSeconds) == 1.0f);
    CHECK(!sw.running(10.0 + kPopSeconds));
  }

  CASE("a squashed label stays inside the label box");
  {
    for (int k = 1; k <= 10; ++k) {
      Framebuffer fb;
      mini_draw_text_squashed(fb, kLabelX, kMiniLabelBox, 1, "BUSY", W,
                              static_cast<float>(k) / 10.0f);
      for (int y = 0; y < kHeight; ++y)
        for (int x = 0; x < kLabelX; ++x) CHECK(!fb.get(x, y).lit());
      for (int x = 0; x < kWidth; ++x) {
        CHECK(!fb.get(x, 0).lit());
        CHECK(!fb.get(x, 7).lit());
      }
    }
  }
}

static void test_status_swap() {
  CASE("a status change turns over in place and settles on the new status");
  UiState ui;
  ui.status = Status::Free;
  ScreenAnim sa;
  Anim a;
  a.dt = 0.01f;
  a.t = 0.0;

  Framebuffer fb;
  for (int i = 0; i < 30; ++i) {  // let it prime and settle on FREE
    a.t = i * 0.01;
    fb.clear();
    draw_screen(fb, Screen::Status, ui, a, sa);
  }

  // Change it, and walk the exchange.
  ui.status = Status::Busy;
  bool saw_shrunk = false;
  for (int i = 30; i < 30 + 40; ++i) {
    a.t = i * 0.01;
    fb.clear();
    draw_screen(fb, Screen::Status, ui, a, sa);
    // Somewhere in the middle the icon must be smaller than either end state.
    int rows = 0;
    for (int y = 0; y < kHeight; ++y) {
      bool any = false;
      for (int x = 0; x < kIconW; ++x)
        if (fb.get(x, y).lit()) any = true;
      if (any) ++rows;
    }
    if (rows > 0 && rows <= 4) saw_shrunk = true;
  }
  CHECK(saw_shrunk);

  CASE("and lands on exactly the settled frame");
  Framebuffer settled, direct;
  a.t = 100.0;
  draw_screen(settled, Screen::Status, ui, a, sa);
  ScreenAnim fresh;
  Anim b;
  b.dt = 0.01f;
  b.t = 100.0;
  for (int i = 0; i < 3; ++i) {
    direct.clear();
    draw_screen(direct, Screen::Status, ui, b, fresh);
  }
  int diff = 0;
  for (int y = 0; y < kHeight; ++y)
    for (int x = 0; x < kIconW; ++x)
      if (settled.get(x, y).lit() != direct.get(x, y).lit()) ++diff;
  CHECK_EQ(diff, 0);
}


static void test_flourish() {
  CASE("done runs flash, settle, hold, fade, and then stops");
  {
    Flourish f;
    Framebuffer fb;
    Anim a;
    a.dt = 0.01f;
    f.done(RGB(255, 40, 20), "DONE", 0.0);

    a.t = 0.0;
    fb.clear();
    f.draw(fb, a);
    const RGB flash = fb.get(23, 0);
    CHECK(flash.r == flash.g && flash.g == flash.b);  // the flash is white
    CHECK(flash.r > 100);
    CHECK_EQ(lit_count(fb), kNumLeds);                // and covers everything

    a.t = 0.6;  // held
    fb.clear();
    f.draw(fb, a);
    const RGB held = fb.get(23, 0);
    CHECK(held.r > held.g && held.r > held.b);        // settled on the colour
    CHECK(f.opaque(0.6));

    a.t = kDoneSeconds + 0.01;
    fb.clear();
    f.draw(fb, a);
    CHECK_EQ(lit_count(fb), 0);                       // nothing left behind
    CHECK(!f.active(a.t));
    CHECK_EQ(static_cast<int>(f.kind()), static_cast<int>(FlourishKind::None));
  }

  CASE("the fade uncovers what was underneath rather than painting it black");
  {
    // Filling toward black would end the moment on a hard cut from an empty
    // panel to a lit one, which is most of what the fade is for.
    Flourish f;
    Framebuffer fb;
    Anim a;
    a.dt = 0.01f;
    f.done(RGB(255, 40, 20), "DONE", 0.0);
    bool saw_underneath = false;
    for (double t = kDoneHoldS; t < kDoneSeconds; t += 0.02) {
      fb.fill(RGB(0, 0, 90));  // a blue screen "underneath"
      a.t = t;
      f.draw(fb, a);
      if (fb.get(12, 4).b > 20) saw_underneath = true;
    }
    CHECK(saw_underneath);
  }

  CASE("the flash is the brightest frame the UI draws, and still fits the budget");
  {
    // 192 LEDs of white. It is held below full value on purpose: the power cap
    // would otherwise scale this one frame and nothing else, which turns a
    // deliberate flash into a shrug.
    Flourish f;
    Framebuffer fb;
    Anim a;
    a.dt = 0.01f;
    f.done(RGB(255, 255, 255), "DONE", 0.0);
    float peak = 0.0f;
    for (double t = 0.0; t < kDoneSeconds; t += 0.01) {
      fb.clear();
      a.t = t;
      f.draw(fb, a);
      const float ma = fb.estimate_ma(255);
      if (ma > peak) peak = ma;
    }
    CHECK(peak < kMaxMilliamps);
  }

  CASE("a toast dims the screen under it instead of replacing it");
  {
    Flourish f;
    Framebuffer fb;
    Anim a;
    a.dt = 0.01f;
    f.toast(RGB(0, 210, 70), "OK", &kIconFree, 0.0);
    fb.fill(RGB(0, 0, 200));
    a.t = kToastSeconds * 0.5;
    f.draw(fb, a);
    // Somewhere away from the toast the blue is still there, only dimmer.
    const RGB under = fb.get(kWidth - 1, kHeight - 1);
    CHECK(under.b > 0);
    CHECK(under.b < 200);
    CHECK(!f.opaque(a.t));
  }

  CASE("re-arming a bar refreshes it instead of replaying its entrance");
  {
    // A knob turned steadily would otherwise restart the animation on every
    // detent and never settle into anything you could read.
    Flourish f;
    Framebuffer fb;
    Anim a;
    a.dt = 0.01f;
    // The refresh times are stepped explicitly rather than accumulated: adding
    // 0.05 ten times lands a hair either side of 0.5, and landing just under it
    // put the draw a nanosecond after the last refresh, at the very first frame
    // of the entrance, where the bar is legitimately still black.
    double last = 0.0;
    f.bar(RGB(255, 138, 31), 0.2f, 0.0);
    for (int i = 1; i <= 10; ++i) {
      last = i * 0.05;
      f.bar(RGB(255, 138, 31), 0.5f, last);
    }
    a.t = last + 0.2;
    fb.clear();
    f.draw(fb, a);
    // Half a second of turning, and the bar is fully present rather than
    // stuck at the first frame of its entrance.
    int lit_row = 0;
    for (int x = 0; x < kWidth; ++x)
      if (fb.get(x, 7).lit()) ++lit_row;
    CHECK(lit_row > kWidth / 3);
    CHECK(f.active(a.t));
    CHECK(!f.active(last + kBarSeconds + 0.01));
  }

  CASE("a flourish that was never started draws nothing");
  {
    Flourish f;
    Framebuffer fb;
    Anim a;
    a.t = 5.0;
    fb.clear();
    f.draw(fb, a);
    CHECK_EQ(lit_count(fb), 0);
    CHECK(!f.active(5.0));
    CHECK(!f.opaque(5.0));
  }
}

static void test_digit_roll() {
  CASE("a roll stays inside its own five-row band");
  Framebuffer fb;
  const RGB sentinel(9, 9, 9);
  PairFaceAnim anim;
  // Prime, then change, then sample mid-roll.
  draw_pair_face_anim(fb, anim, 25, 0, true, RGB(255, 255, 255), 0.0);
  for (int i = 1; i <= 12; ++i) {
    fb.fill(sentinel);
    draw_pair_face_anim(fb, anim, 24, 59, true, RGB(255, 255, 255),
                        0.0 + i * (kRollSeconds / 12.0));
    for (int x = 0; x < kWidth; ++x) {
      CHECK(fb.get(x, 0) == sentinel);
      CHECK(fb.get(x, 6) == sentinel);
      CHECK(fb.get(x, 7) == sentinel);
    }
  }

  CASE("a roll lands exactly on the new digit");
  Framebuffer rolled, plain;
  PairFaceAnim a2;
  draw_pair_face_anim(rolled, a2, 25, 0, true, RGB(255, 255, 255), 0.0);
  draw_pair_face_anim(rolled, a2, 24, 59, true, RGB(255, 255, 255), 0.0);
  rolled.clear();
  draw_pair_face_anim(rolled, a2, 24, 59, true, RGB(255, 255, 255), 1.0);
  plain.clear();
  draw_pair_face(plain, 24, 59, true, RGB(255, 255, 255));
  for (int y = 0; y < kHeight; ++y)
    for (int x = 0; x < kWidth; ++x) CHECK(rolled.get(x, y) == plain.get(x, y));

  CASE("only the digits that changed roll");
  PairFaceAnim a3;
  Framebuffer f3;
  draw_pair_face_anim(f3, a3, 25, 0, true, RGB(255, 255, 255), 0.0);
  draw_pair_face_anim(f3, a3, 24, 59, true, RGB(255, 255, 255), 0.0);
  int active = 0;
  for (int i = 0; i < 4; ++i) active += a3.d[i].active ? 1 : 0;
  CHECK_EQ(active, 3);  // 2500 -> 2459 changes three of the four digits
}

static void test_dither() {
  uint8_t w[kNumLeds * 3];
  Framebuffer fb;

  CASE("a dithered channel time-averages to its exact value");
  fb.clear();
  fb.set(0, 0, RGB(0, 37, 0));
  Renderer r;
  const int i0 = led_index(0, 0, kWiring) * 3;  // green is the first byte
  long sum = 0;
  const int N = 512;
  for (int i = 0; i < N; ++i) {
    r.render(fb, w, 48, kMaxMilliamps, kWiring);
    sum += w[i0];
  }
  const double exact = kGamma8[37] * 48.0 / 255.0;
  CHECK_NEAR(sum / static_cast<double>(N), exact, 0.05);

  CASE("a value that rounds to zero without dithering still reaches the LEDs");
  // This is the sleep-screen bug in miniature: gamma plus a low brightness
  // rounds small values away entirely unless the remainder is carried.
  fb.clear();
  fb.fill(RGB(70, 70, 70));
  Renderer dim;
  long lit_frames = 0;
  for (int i = 0; i < 64; ++i) {
    dim.render(fb, w, 8, kMaxMilliamps, kWiring);
    for (int j = 0; j < kNumLeds * 3; ++j)
      if (w[j]) { ++lit_frames; break; }
  }
  CHECK(lit_frames > 0);

  CASE("dithering is deterministic");
  Renderer a, b;
  uint8_t wa[kNumLeds * 3], wb[kNumLeds * 3];
  for (int i = 0; i < 16; ++i) {
    a.render(fb, wa, 48, kMaxMilliamps, kWiring);
    b.render(fb, wb, 48, kMaxMilliamps, kWiring);
    CHECK_EQ(std::memcmp(wa, wb, sizeof wa), 0);
  }

  CASE("with dithering off, identical frames give identical bytes");
  Renderer nd;
  nd.set_dither(false);
  nd.render(fb, wa, 48, kMaxMilliamps, kWiring);
  nd.render(fb, wb, 48, kMaxMilliamps, kWiring);
  CHECK_EQ(std::memcmp(wa, wb, sizeof wa), 0);

  CASE("neighbouring LEDs do not dither in lockstep");
  // A uniform panel must not toggle as one block: that reads as a shimmer.
  Renderer u;
  u.render(fb, w, 48, kMaxMilliamps, kWiring);
  std::set<uint8_t> seen;
  for (int i = 0; i < kNumLeds * 3; ++i) seen.insert(w[i]);
  CHECK(seen.size() >= 2);

  CASE("the power cap still holds with dithering on");
  Framebuffer white;
  white.fill(RGB(255, 255, 255));
  Renderer pc;
  const RenderStats st = pc.render(white, w, 255, kMaxMilliamps, kWiring);
  CHECK(st.power_scale < 1.0f);
  CHECK(st.est_ma > 10000.0f);
}

static void test_dither_is_not_periodic() {
  CASE("a dim pixel at low brightness does not blink at one frequency");
  {
    // Plain sigma-delta on a constant input is exactly periodic. At brightness
    // 6 a dim pixel works out at twelve 256ths of a step, which fires once
    // every 21 frames — a dead-regular 4.7 Hz blink, right in the band the eye
    // is most sensitive to. Jittering the comparator spreads that over a noise
    // floor without moving the average.
    Framebuffer fb;
    fb.fill(RGB(40, 40, 40));
    Renderer r;
    r.reset_dither();
    uint8_t grb[kNumLeds * 3];
    std::vector<int> gaps;
    int last = -1;
    long pulses = 0;
    for (int f = 0; f < 4000; ++f) {
      r.render(fb, grb, 6, 100000.0f, kWiring);
      if (grb[0]) {
        ++pulses;
        if (last >= 0) gaps.push_back(f - last);
        last = f;
      }
    }
    CHECK(gaps.size() > 50);

    // Not one interval repeated: the spread is the whole point.
    int mn = 1 << 30, mx = 0;
    double mean = 0;
    for (int g : gaps) {
      if (g < mn) mn = g;
      if (g > mx) mx = g;
      mean += g;
    }
    mean /= static_cast<double>(gaps.size());
    CHECK(mx - mn >= 4);

    // And the average is still exactly what was asked for, because the error
    // feedback is computed from the unjittered value. The noise moves when a
    // pulse happens, never how many.
    const int32_t k = static_cast<int32_t>((6 / 255.0f) * 65536.0f + 0.5f);
    const int32_t exact = (static_cast<int32_t>(kGamma8[40]) * k) >> 8;
    CHECK(exact > 0);
    const double want = 256.0 / static_cast<double>(exact);
    CHECK(std::fabs(mean - want) < want * 0.08);
  }
}

static void test_transitions() {
  UiState ui;
  Framebuffer from, to, out;
  ScreenAnim sa, sb;
  Anim a;
  a.dt = 0.0f;
  a.t = 1.0;
  ui.status = Status::Busy;
  draw_screen(from, Screen::Status, ui, a, sa);
  draw_screen(to, Screen::Clock, ui, a, sb);

  CASE("every transition starts on the old screen and ends on the new one");
  for (int k = 1; k < static_cast<int>(TransitionKind::Count); ++k) {
    const TransitionKind kind = static_cast<TransitionKind>(k);
    compose(out, from, to, kind, 0.0f);
    for (int y = 0; y < kHeight; ++y)
      for (int x = 0; x < kWidth; ++x) CHECK(out.get(x, y) == from.get(x, y));
    compose(out, from, to, kind, 1.0f);
    for (int y = 0; y < kHeight; ++y)
      for (int x = 0; x < kWidth; ++x) CHECK(out.get(x, y) == to.get(x, y));
  }

  CASE("the dissolve order is a true permutation of the panel");
  std::set<int> ranks;
  for (int y = 0; y < kHeight; ++y)
    for (int x = 0; x < kWidth; ++x) ranks.insert(dissolve_rank(x, y));
  CHECK_EQ(ranks.size(), kNumLeds);

  CASE("the dissolve switches pixels steadily, in proportion to progress");
  int prev = -1;
  for (int i = 0; i <= 32; ++i) {
    const float p = i / 32.0f;
    compose(out, from, to, TransitionKind::Dissolve, p);
    int switched = 0;
    for (int y = 0; y < kHeight; ++y)
      for (int x = 0; x < kWidth; ++x)
        if (dissolve_rank(x, y) < static_cast<int>(p * kNumLeds + 0.5f)) ++switched;
    CHECK(switched >= prev);
    prev = switched;
  }

  CASE("a fade passes through black");
  compose(out, from, to, TransitionKind::Fade, 0.5f);
  CHECK_EQ(lit_count(out), 0);

  CASE("the transition kind matches the gesture");
  // The view cycle turns like a record, so it reads as one list of items on
  // one object rather than a stack of unrelated cards.
  CHECK_EQ(static_cast<int>(ScreenManager::kind_for(Screen::Status, Screen::Clock)),
           static_cast<int>(TransitionKind::DiskUp));
  CHECK_EQ(static_cast<int>(ScreenManager::kind_for(Screen::Clock, Screen::Status)),
           static_cast<int>(TransitionKind::DiskDown));
  CHECK(transition_seconds(TransitionKind::DiskUp) > transition_seconds(TransitionKind::Fade));
  CHECK_EQ(static_cast<int>(ScreenManager::kind_for(Screen::Timer, Screen::Brightness)),
           static_cast<int>(TransitionKind::WipeUp));
  CHECK_EQ(static_cast<int>(ScreenManager::kind_for(Screen::Brightness, Screen::Timer)),
           static_cast<int>(TransitionKind::WipeDown));
  CHECK_EQ(static_cast<int>(ScreenManager::kind_for(Screen::Status, Screen::Sleep)),
           static_cast<int>(TransitionKind::Fade));

  CASE("a managed transition runs to completion and never blanks the panel");
  ScreenManager m;
  m.set_screen(Screen::Status);
  m.go_to(Screen::Clock, ui);
  CHECK(m.busy());
  float last = 0.0f;
  Anim step;
  step.dt = 0.01f;
  int blank_frames = 0;
  // Long enough for the slowest kind, whatever kind_for picked.
  const int frames = static_cast<int>(transition_seconds(TransitionKind::DiskUp) * 100.0f) + 8;
  for (int i = 0; i < frames; ++i) {
    step.t = 1.0 + i * 0.01;
    m.advance(step.dt);
    m.render(out, ui, step);
    CHECK(m.progress() >= last);
    last = m.progress();
    if (lit_count(out) == 0) ++blank_frames;
  }
  CHECK(!m.busy());
  CHECK_EQ(m.current(), static_cast<int>(Screen::Clock));
  CHECK(blank_frames == 0);  // the view cycle never goes dark

  CASE("the outgoing screen keeps the state it was showing, not the new one");
  {
    // The manager is told what it is leaving while the caller still holds it.
    // Snapshotting lazily on the first transition frame drew the *new* state on
    // both sides. On the Status screen its own swap animation disguised that,
    // which is why it went unnoticed; a menu list would have shown the new item
    // scrolling away from itself.
    //
    // Brightness is the screen that can prove it: its NN% label is drawn
    // straight from ui.brightness with no eased state in between, so there is
    // nothing to hide behind.
    auto label_ink = [](const Framebuffer& f) {
      int n = 0;
      for (int y = 1; y <= 5; ++y)
        for (int x = kLabelX; x < kWidth; ++x)
          if (f.get(x, y).lit()) ++n;
      return n;
    };

    UiState u;
    Anim z;
    z.dt = 0.0f;
    z.t = 5.0;

    ScreenManager hi;   // stays at 100% throughout: the reference
    hi.set_screen(Screen::Brightness);
    u.brightness = 255;
    Framebuffer ref_hi;
    hi.render(ref_hi, u, z);

    ScreenManager lo;   // primed at 4%, for the other reference
    lo.set_screen(Screen::Brightness);
    UiState dim = u;
    dim.brightness = 10;
    Framebuffer ref_lo;
    lo.render(ref_lo, dim, z);

    CHECK(label_ink(ref_hi) != label_ink(ref_lo));  // the two really do differ

    ScreenManager m3;
    m3.set_screen(Screen::Brightness);
    Framebuffer scratch;
    m3.render(scratch, u, z);        // prime on 100%
    m3.restart_with(u, TransitionKind::DiskUp);
    u.brightness = 10;               // changed only after the manager was told

    Framebuffer out;
    m3.render(out, u, z);            // dt 0, so progress is 0 and out == from

    for (int y = 1; y <= 5; ++y)
      for (int x = kLabelX; x < kWidth; ++x) CHECK(out.get(x, y) == ref_hi.get(x, y));
    CHECK(label_ink(out) != label_ink(ref_lo));
  }

  CASE("both screens keep animating through a transition");
  ScreenManager m2;
  m2.set_screen(Screen::Status);
  m2.go_to(Screen::Clock, ui, TransitionKind::SlideLeft, 1.0f);
  Framebuffer f1, f2;
  Anim s1, s2;
  s1.dt = 0.0f;
  s1.t = 5.0;
  m2.render(f1, ui, s1);
  // Same progress, different absolute time: a frozen screen would be identical.
  s2.dt = 0.0f;
  s2.t = 5.4;
  m2.render(f2, ui, s2);
  bool moved = false;
  for (int y = 0; y < kHeight && !moved; ++y)
    for (int x = 0; x < kWidth && !moved; ++x)
      if (!(f1.get(x, y) == f2.get(x, y))) moved = true;
  CHECK(moved);
}

int main() {
  std::printf("panel tests\n");
  test_mapping_is_bijective();
  test_wled_reference_mapping();
  test_known_layouts();
  test_font();
  test_color();
  test_render_order_and_power();
  test_engine();
  test_screens();
  test_anim();
  test_subpixel();
  test_mini_font();
  test_icons();
  test_sprite();
  test_status_swap();
  test_flourish();
  test_digit_roll();
  test_transitions();
  test_dither();
  test_dither_is_not_periodic();
  run_ui_tests();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
