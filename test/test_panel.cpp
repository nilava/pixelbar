// Host-side tests for the panel component. No framework: a few macros and a
// non-zero exit code on failure.
//
//   ./test/run.sh
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>

#include "panel/config.h"
#include "panel/font.h"
#include "panel/framebuffer.h"
#include "panel/patterns.h"
#include "panel/screens.h"

using namespace panel;

static int g_failures = 0;
static int g_checks = 0;
static const char* g_case = "";

#define CASE(name) g_case = name
#define CHECK(cond)                                                             \
  do {                                                                          \
    ++g_checks;                                                                 \
    if (!(cond)) {                                                              \
      ++g_failures;                                                             \
      std::printf("  FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, g_case, #cond); \
    }                                                                           \
  } while (0)
#define CHECK_EQ(a, b)                                                     \
  do {                                                                     \
    ++g_checks;                                                            \
    const long _a = static_cast<long>(a), _b = static_cast<long>(b);       \
    if (_a != _b) {                                                        \
      ++g_failures;                                                        \
      std::printf("  FAIL %s:%d [%s] %s == %s (%ld vs %ld)\n", __FILE__,   \
                  __LINE__, g_case, #a, #b, _a, _b);                       \
    }                                                                      \
  } while (0)
#define CHECK_NEAR(a, b, tol)                                              \
  do {                                                                     \
    ++g_checks;                                                            \
    const double _a = (a), _b = (b);                                       \
    if (std::fabs(_a - _b) > (tol)) {                                      \
      ++g_failures;                                                        \
      std::printf("  FAIL %s:%d [%s] %s ~= %s (%.3f vs %.3f)\n", __FILE__, \
                  __LINE__, g_case, #a, #b, _a, _b);                       \
    }                                                                      \
  } while (0)

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
  CHECK(kGamma8[255] > 200);
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
  for (int i = 0; i < static_cast<int>(Status::Count); ++i) {
    const char* label = status_label(static_cast<Status>(i));
    CHECK(text_fits(label));
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
  bool found_busy = false;
  for (int y = 0; y < kHeight; ++y)
    for (int x = 0; x < kWidth; ++x)
      if (fb.get(x, y) == status_color(Status::Busy)) found_busy = true;
  CHECK(found_busy);

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
      if (c.lit() && c.r > 200 && c.g < 80) any_red = true;
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
  ui.hue = 0.0f;
  draw_screen(fb, Screen::ColorPick, ui, 0);
  CHECK(fb.get(0, 7) == RGB(255, 255, 255));
  ui.hue = 1.0f;
  draw_screen(fb, Screen::ColorPick, ui, 0);
  CHECK(fb.get(kWidth - 1, 7) == RGB(255, 255, 255));

  CASE("sleep shows one dim pixel and nothing else");
  draw_screen(fb, Screen::Sleep, ui, 0);
  CHECK_EQ(lit_count(fb), 1);
  CHECK(fb.get(0, kHeight - 1).r < 20);

  CASE("screen names round-trip to something printable");
  for (int i = 0; i < static_cast<int>(Screen::Count); ++i) {
    const char* n = screen_name(static_cast<Screen>(i));
    CHECK(n != nullptr && n[0] != '?');
  }
}

int main() {
  std::printf("panel tests\n");
  test_mapping_is_bijective();
  test_known_layouts();
  test_font();
  test_color();
  test_render_order_and_power();
  test_engine();
  test_screens();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
