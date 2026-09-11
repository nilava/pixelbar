// The panel, on your terminal, driven by your keyboard.
//
//     ./test/sim.sh
//
// This runs the real state machine, the real gesture recogniser, the real
// screen manager and the real renderer at 100 fps, and draws the result out of
// the same GRB wire bytes that would go down the data line — through the same
// LED mapping and the same gamma table. A mapping or gamma mistake therefore
// shows up here rather than on a soldered panel.
//
// Keystrokes are turned into raw pad levels and encoder counts, not into
// events, so the recogniser is exercised for real: a tap here is a pad going
// down and coming back up over several frames, and a swipe is a scripted drag
// with the overlaps a finger actually makes.
//
// Nothing but libc. No curses, no SDL.
#include <termios.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "panel/anim.h"
#include "panel/config.h"
#include "panel/framebuffer.h"
#include "panel/geometry.h"
#include "panel/renderer.h"
#include "ui/app.h"

using namespace panel;

namespace {

constexpr int kFrameUs = 1000000 / kFramesPerSecond;

// ----------------------------------------------------------------- terminal

termios g_saved;
bool g_raw = false;

void restore_terminal() {
  if (!g_raw) return;
  tcsetattr(STDIN_FILENO, TCSANOW, &g_saved);
  std::printf("\x1b[?25h\x1b[0m\n");  // cursor back, colours off
  std::fflush(stdout);
  g_raw = false;
}

bool raw_terminal() {
  if (!isatty(STDIN_FILENO)) return false;
  if (tcgetattr(STDIN_FILENO, &g_saved) != 0) return false;
  termios t = g_saved;
  t.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
  t.c_cc[VMIN] = 0;   // never block: this is a render loop, not a prompt
  t.c_cc[VTIME] = 0;
  if (tcsetattr(STDIN_FILENO, TCSANOW, &t) != 0) return false;
  g_raw = true;
  std::printf("\x1b[?25l");  // hide the cursor
  return true;
}

void sleep_us(long us) {
  timespec ts;
  ts.tv_sec = us / 1000000;
  ts.tv_nsec = (us % 1000000) * 1000;
  nanosleep(&ts, nullptr);
}

int64_t now_us() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

// ------------------------------------------------------------------- ports
//
// Keystrokes schedule pad presses that last a realistic number of frames,
// because a terminal has no key-up. Holding is a separate, explicit toggle.

class SimPorts : public ui::Ports {
 public:
  void read_raw(ui::RawInput* out) override {
    for (int i = 0; i < ui::kZones; ++i) out->touch[i] = touch_[i] || latched_[i];
    out->encoder_sw = sw_ || sw_latched_;
    out->encoder_detents = detents_;
    out->motion_valid = true;
    // Standing on its front edge: gravity lies in the panel plane. Laid flat
    // it moves onto the board normal. The jolt is added to whichever axis is
    // carrying gravity, so a knock reads the same in either pose.
    const float j = jolt_;
    if (flat_) {
      out->ax = 0.0f;
      out->ay = 0.0f;
      out->az = 1.0f + j;
    } else {
      out->ax = 0.0f;
      out->ay = 1.0f + j;
      out->az = 0.0f;
    }
  }

  bool load_settings(ui::Settings* out) override {
    if (!stored_valid_) return false;
    *out = stored_;
    return true;
  }
  bool save_settings(const ui::Settings& s) override {
    stored_ = s;
    stored_valid_ = true;
    ++saves_;
    return true;
  }
  bool wall_clock(int* h, int* m, int* s) override {
    // A synthetic clock that actually moves, so the Clock screen has something
    // to animate. Starts at 14:25 like the preview sheets.
    const int t = 14 * 3600 + 25 * 60 + static_cast<int>(uptime_s_);
    *h = (t / 3600) % 24;
    *m = (t / 60) % 60;
    *s = t % 60;
    return true;
  }
  bool wifi_connected() override { return wifi_; }

  // ---- driven by the keyboard

  void tap(int zone) {
    if (zone < 0 || zone >= ui::kZones) return;
    touch_[zone] = true;
    hold_frames_[zone] = 6;  // 60 ms, a realistic quick tap
  }
  void toggle_hold(int zone) {
    if (zone < 0 || zone >= ui::kZones) return;
    latched_[zone] = !latched_[zone];
  }
  void press(int frames = 6) {
    sw_ = true;
    sw_frames_ = frames;
  }
  void toggle_knob() { sw_latched_ = !sw_latched_; }
  void turn(int d) { detents_ += d; }
  void knock(float g) { jolt_pending_ = g; }
  void toggle_flat() { flat_ = !flat_; }
  void toggle_wifi() { wifi_ = !wifi_; }
  // A finger dragged across the pads, with the overlaps a real one makes.
  void drag(int dir) {
    drag_dir_ = dir;
    drag_step_ = 0;
    drag_timer_ = 0;
  }

  // Advances the scheduled input by one frame. Called before App::update.
  void advance(float dt_s) {
    uptime_s_ += dt_s;
    for (int i = 0; i < ui::kZones; ++i) {
      if (hold_frames_[i] > 0 && --hold_frames_[i] == 0) touch_[i] = false;
    }
    if (sw_frames_ > 0 && --sw_frames_ == 0) sw_ = false;

    // The jolt lasts exactly one frame, which is what a knock looks like.
    jolt_ = jolt_pending_;
    jolt_pending_ = 0.0f;

    if (drag_step_ >= 0) {
      static const int kSeqR[3] = {0, 1, 2};
      static const int kSeqL[3] = {2, 1, 0};
      const int* seq = drag_dir_ > 0 ? kSeqR : kSeqL;
      // down a, (down b, up a), (down c, up b), up c
      ++drag_timer_;
      if (drag_timer_ == 1) {
        touch_[seq[0]] = true;
      } else if (drag_timer_ == 6) {
        touch_[seq[1]] = true;
      } else if (drag_timer_ == 9) {
        touch_[seq[0]] = false;
      } else if (drag_timer_ == 12) {
        touch_[seq[2]] = true;
      } else if (drag_timer_ == 15) {
        touch_[seq[1]] = false;
      } else if (drag_timer_ >= 21) {
        touch_[seq[2]] = false;
        drag_step_ = -1;
      }
    }
  }

  int saves() const { return saves_; }
  bool flat() const { return flat_; }
  bool knob_latched() const { return sw_latched_; }
  uint8_t latched_mask() const {
    uint8_t m = 0;
    for (int i = 0; i < ui::kZones; ++i)
      if (latched_[i]) m |= static_cast<uint8_t>(1u << i);
    return m;
  }

 private:
  bool touch_[ui::kZones] = {false, false, false};
  bool latched_[ui::kZones] = {false, false, false};
  int hold_frames_[ui::kZones] = {0, 0, 0};
  bool sw_ = false, sw_latched_ = false;
  int sw_frames_ = 0;
  int32_t detents_ = 0;
  float jolt_ = 0.0f, jolt_pending_ = 0.0f;
  bool flat_ = false;
  bool wifi_ = false;
  int drag_step_ = -1, drag_timer_ = 0, drag_dir_ = 1;
  double uptime_s_ = 0.0;
  ui::Settings stored_;
  bool stored_valid_ = false;
  int saves_ = 0;
};

// ------------------------------------------------------------------ drawing

// Two panel rows per character cell: the upper half-block carries the top row
// as its foreground and the bottom row as its background.
void draw_panel(const uint8_t* grb, char* buf, size_t cap) {
  size_t n = 0;
  auto put = [&](const char* s) {
    const size_t len = std::strlen(s);
    if (n + len < cap) {
      std::memcpy(buf + n, s, len);
      n += len;
    }
  };
  char tmp[64];

  for (int y = 0; y < kHeight; y += 2) {
    put("  ");
    for (int x = 0; x < kWidth; ++x) {
      const int hi = led_index(x, y, kWiring) * 3;
      const int lo = led_index(x, y + 1, kWiring) * 3;
      std::snprintf(tmp, sizeof(tmp), "\x1b[38;2;%d;%d;%dm\x1b[48;2;%d;%d;%dm▀",
                    grb[hi + 1], grb[hi + 0], grb[hi + 2],
                    grb[lo + 1], grb[lo + 0], grb[lo + 2]);
      put(tmp);
    }
    put("\x1b[0m\n");
  }
  buf[n < cap ? n : cap - 1] = 0;
}

const char* status_word(Status s) { return status_label(s); }

void print_help() {
  std::printf(
      "\x1b[2J\x1b[H"
      "  pixelbar simulator\n\n"
      "   a s d   tap the left / middle / right pad\n"
      "   A S D   hold a pad (toggles, so you can chord)\n"
      "   e E     swipe across the pads, rightward / leftward\n"
      "   , .     turn the knob one detent, anticlockwise / clockwise\n"
      "   < >     turn it five at once, fast enough to trigger acceleration\n"
      "   space   press the knob\n"
      "   p       hold the knob down (toggles, for press-and-turn)\n"
      "   m       press and hold the knob\n"
      "   t       knock the case\n"
      "   k       shake it\n"
      "   f       lay it flat / stand it up\n"
      "   n       pretend WiFi came up\n"
      "   ?       this help        q  quit\n\n"
      "  press any key to start\n");
  std::fflush(stdout);
}

}  // namespace

// Runs a scripted sequence with no terminal, printing the panel at each step.
//
// This exists so the simulator itself is provable: the glue between keystroke,
// recogniser, model and renderer is the part most likely to be silently wrong,
// and it cannot be exercised from a test that has no tty. Piping this into a
// file also makes a decent before-and-after when a screen changes.
int selftest() {
  struct Step {
    const char* what;
    const char* keys;
    float settle_s;
  };
  static const Step kScript[] = {
      {"boot", "", 2.0f},
      {"tap left: busy", "a", 1.2f},
      {"tap left twice: call", "aa", 1.2f},
      {"tap right: clock", "d", 1.2f},
      {"swipe right: timer", "e", 1.5f},
      {"tap middle: running", "s", 0.6f},
      {"hold the knob: brightness", "m", 1.5f},
      {"turn it up", ">>>>", 1.0f},
      {"press: colour", " ", 1.2f},
      {"turn the hue", ">>>>>>", 1.0f},
      {"chord left+right: sleep", "AD", 1.5f},
  };

  SimPorts ports;
  ui::App app;
  Renderer renderer;
  Framebuffer fb;
  FrameClock clock;
  uint8_t grb[kNumLeds * 3];
  char panel_buf[16384];
  app.begin(ports, 0.0);

  micros_t t = 0;
  auto advance = [&](float seconds) {
    const int frames = static_cast<int>(seconds * kFramesPerSecond);
    for (int i = 0; i < frames; ++i) {
      t += kFrameUs;
      const Anim a = clock.tick(t);
      ports.advance(a.dt);
      app.update(a.dt, a.t);
      app.render(fb, a);
      renderer.render(fb, grb, app.state().brightness, kMaxMilliamps, kWiring);
    }
  };

  for (const Step& st : kScript) {
    for (const char* k = st.keys; *k; ++k) {
      switch (*k) {
        case 'a': ports.tap(0); break;
        case 's': ports.tap(1); break;
        case 'd': ports.tap(2); break;
        case 'A': ports.toggle_hold(0); break;
        case 'D': ports.toggle_hold(2); break;
        case 'e': ports.drag(+1); break;
        case '>': ports.turn(+5); break;
        case ' ': ports.press(); break;
        case 'm': ports.press(60); break;
        default: break;
      }
      advance(0.30f);  // let each keystroke play out before the next
    }
    advance(st.settle_s);
    draw_panel(grb, panel_buf, sizeof(panel_buf));
    const UiState& ui = app.state();
    std::printf("%s\n%s  \x1b[2m%s  %s  %d%%\x1b[0m\n\n", st.what, panel_buf,
                screen_name(app.screen()), status_word(ui.status),
                (ui.brightness * 100 + 127) / 255);
  }
  std::printf("  settings written: %d\n", ports.saves());
  return 0;
}

int main(int argc, char** argv) {
  if (argc > 1 && std::strcmp(argv[1], "--help") == 0) {
    print_help();
    return 0;
  }
  if (argc > 1 && std::strcmp(argv[1], "--selftest") == 0) return selftest();
  if (!raw_terminal()) {
    std::fprintf(stderr,
                 "sim needs an interactive terminal (stdin is not a tty)\n");
    return 1;
  }
  std::atexit(restore_terminal);

  SimPorts ports;
  ui::App app;
  Renderer renderer;
  Framebuffer fb;
  FrameClock clock;
  FpsMeter meter;
  uint8_t grb[kNumLeds * 3];

  const int64_t t0 = now_us();
  app.begin(ports, 0.0);

  bool running = true;
  bool paused = false;
  float speed = 1.0f;
  char panel_buf[16384];
  int64_t next_frame = now_us();

  std::printf("\x1b[2J");
  while (running) {
    // ---- input
    char c;
    while (read(STDIN_FILENO, &c, 1) == 1) {
      switch (c) {
        case 'q': running = false; break;
        case 'a': ports.tap(0); break;
        case 's': ports.tap(1); break;
        case 'd': ports.tap(2); break;
        case 'A': ports.toggle_hold(0); break;
        case 'S': ports.toggle_hold(1); break;
        case 'D': ports.toggle_hold(2); break;
        case 'e': ports.drag(+1); break;
        case 'E': ports.drag(-1); break;
        case ',': ports.turn(-1); break;
        case '.': ports.turn(+1); break;
        case '<': ports.turn(-5); break;
        case '>': ports.turn(+5); break;
        case ' ': ports.press(); break;
        case 'p': ports.toggle_knob(); break;
        case 'm': ports.press(60); break;  // 600 ms, past the hold threshold
        case 't': ports.knock(1.4f); break;
        case 'k': ports.knock(2.2f); break;
        case 'f': ports.toggle_flat(); break;
        case 'n': ports.toggle_wifi(); break;
        case '[': speed = speed > 0.2f ? speed * 0.5f : speed; break;
        case ']': speed = speed < 4.0f ? speed * 2.0f : speed; break;
        case 'P': paused = !paused; break;
        default: break;
      }
    }

    // ---- advance
    const int64_t t_us = now_us() - t0;
    const Anim a_raw = clock.tick(static_cast<micros_t>(t_us));
    Anim a = a_raw;
    if (paused) a.dt = 0.0f;
    a.dt *= speed;

    ports.advance(a.dt);
    app.update(a.dt, a.t);
    app.render(fb, a);
    const RenderStats st =
        renderer.render(fb, grb, app.state().brightness, kMaxMilliamps, kWiring);
    meter.tick(static_cast<micros_t>(t_us));

    // ---- draw
    draw_panel(grb, panel_buf, sizeof(panel_buf));
    const UiState& ui = app.state();
    std::printf(
        "\x1b[H\n%s\n"
        "  \x1b[2m%-10s\x1b[0m  %-5s  %3d%%  %02d:%02d  timer %02d:%02d %-6s"
        "  \x1b[2m%s\x1b[0m\n"
        "  \x1b[2mfps %5.1f   pads %c%c%c  knob %s  %s  %s   "
        "? help   q quit\x1b[0m   \n",
        panel_buf, screen_name(app.screen()), status_word(ui.status),
        (ui.brightness * 100 + 127) / 255, ui.hour, ui.minute,
        ui.timer_left_s / 60, ui.timer_left_s % 60,
        ui.timer_running ? "run" : "stop", ui::event_name(app.last_event()),
        static_cast<double>(meter.fps()),
        (ports.latched_mask() & 1) ? 'L' : '-',
        (ports.latched_mask() & 2) ? 'M' : '-',
        (ports.latched_mask() & 4) ? 'R' : '-',
        ports.knob_latched() ? "down" : "up  ",
        ports.flat() ? "flat " : "up   ",
        st.power_scale < 1.0f ? "POWER CAPPED" : "            ");
    std::fflush(stdout);

    // ---- pace
    next_frame += kFrameUs;
    const int64_t slack = next_frame - now_us();
    if (slack > 0) {
      sleep_us(slack);
    } else {
      next_frame = now_us();  // fell behind; do not spiral
    }
  }

  restore_terminal();
  return 0;
}
