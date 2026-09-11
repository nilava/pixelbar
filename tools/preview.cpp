// Renders the real firmware pipeline to PPM images so patterns can be checked
// on a laptop, with no hardware.
//
// It deliberately reads back the GRB bytes that would go out on the wire, via
// the same led_index() mapping the device uses, so a mapping or gamma mistake
// shows up here rather than on a soldered panel.
//
//   ./test/run.sh && ./build-host/preview out_dir
//   python3 tools/ppm2png.py out_dir
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "panel/config.h"
#include "panel/flourish.h"
#include "panel/framebuffer.h"
#include "panel/patterns.h"
#include "panel/renderer.h"
#include "panel/screens.h"
#include "panel/transition.h"

using namespace panel;

namespace {

constexpr int kPitch = 14;   // cell spacing in the preview image
constexpr int kCell = 12;    // lit area of one LED cell
constexpr int kMargin = 4;
constexpr int kFrameGap = 6;
constexpr int kFramesPerSheet = 4;
constexpr float kGlow = 0.55f;
// Brighter than the device default: a monitor in a lit room does not carry the
// same punch as an LED, and the point of the sheet is to read the shapes.
constexpr uint8_t kPreviewBrightness = 190;

struct Image {
  int w = 0, h = 0;
  std::vector<float> px;  // rgb, 0..255

  Image(int w_, int h_) : w(w_), h(h_), px(static_cast<size_t>(w_) * h_ * 3, 0.0f) {}

  void set(int x, int y, float r, float g, float b) {
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    const size_t i = (static_cast<size_t>(y) * w + x) * 3;
    px[i] = r;
    px[i + 1] = g;
    px[i + 2] = b;
  }
  void add(int x, int y, float r, float g, float b) {
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    const size_t i = (static_cast<size_t>(y) * w + x) * 3;
    px[i] += r;
    px[i + 1] += g;
    px[i + 2] += b;
  }
};

// Separable box blur, used to fake the diffuser's glow.
std::vector<float> blur(const std::vector<float>& src, int w, int h, int radius) {
  std::vector<float> tmp(src.size(), 0.0f), out(src.size(), 0.0f);
  const float norm = 1.0f / (2 * radius + 1);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      float acc[3] = {0, 0, 0};
      for (int k = -radius; k <= radius; ++k) {
        const int xx = std::min(w - 1, std::max(0, x + k));
        const size_t i = (static_cast<size_t>(y) * w + xx) * 3;
        for (int c = 0; c < 3; ++c) acc[c] += src[i + c];
      }
      const size_t o = (static_cast<size_t>(y) * w + x) * 3;
      for (int c = 0; c < 3; ++c) tmp[o + c] = acc[c] * norm;
    }
  }
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      float acc[3] = {0, 0, 0};
      for (int k = -radius; k <= radius; ++k) {
        const int yy = std::min(h - 1, std::max(0, y + k));
        const size_t i = (static_cast<size_t>(yy) * w + x) * 3;
        for (int c = 0; c < 3; ++c) acc[c] += tmp[i + c];
      }
      const size_t o = (static_cast<size_t>(y) * w + x) * 3;
      for (int c = 0; c < 3; ++c) out[o + c] = acc[c] * norm;
    }
  }
  return out;
}

// Draws one frame of LED cells at (ox, oy), reading colours back out of the
// wire-order buffer so the preview exercises the real mapping.
void draw_frame(Image& img, const uint8_t* grb, int ox, int oy) {
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      const int i = led_index(x, y, kWiring) * 3;
      const float g = grb[i + 0];
      const float r = grb[i + 1];
      const float b = grb[i + 2];
      for (int dy = 0; dy < kCell; ++dy) {
        for (int dx = 0; dx < kCell; ++dx) {
          img.set(ox + x * kPitch + dx, oy + y * kPitch + dy, r, g, b);
        }
      }
    }
  }
}

bool write_ppm(const std::string& path, const Image& img) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  std::fprintf(f, "P6\n%d %d\n255\n", img.w, img.h);
  std::vector<uint8_t> row(static_cast<size_t>(img.w) * 3);
  for (int y = 0; y < img.h; ++y) {
    for (int x = 0; x < img.w * 3; ++x) {
      float v = img.px[(static_cast<size_t>(y) * img.w * 3) + x];
      if (v < 0) v = 0;
      if (v > 255) v = 255;
      row[x] = static_cast<uint8_t>(v + 0.5f);
    }
    std::fwrite(row.data(), 1, row.size(), f);
  }
  std::fclose(f);
  return true;
}

void render_sheet(Pattern pat, const std::string& dir) {
  const int fw = kWidth * kPitch + 2 * kMargin;
  const int fh = kHeight * kPitch + 2 * kMargin;
  Image img(fw, kFramesPerSheet * fh + (kFramesPerSheet - 1) * kFrameGap);

  Engine engine;
  engine.set_pattern(pat);
  engine.set_text("BUSY  14:25  FOCUS 25M");
  engine.params.hour = 14;
  engine.params.minute = 25;
  engine.params.second = 37;

  Framebuffer fb;
  uint8_t grb[kNumLeds * 3];
  uint32_t t = 0;
  int drawn = 0;
  // 60 fps, sampling a frame every 0.9 s so consecutive rows visibly differ.
  for (int step = 0; drawn < kFramesPerSheet; ++step) {
    // Run the clock forward too, so the colon blink and the seconds bar move.
    engine.params.second = 37 + static_cast<int>(t / 1000);
    engine.params.minute = 25 + engine.params.second / 60;
    engine.params.second %= 60;
    engine.render(fb, t);
    if (step % 54 == 0) {
      fb.render(grb, kPreviewBrightness, kMaxMilliamps, kWiring);
      draw_frame(img, grb, kMargin, drawn * (fh + kFrameGap) + kMargin);
      ++drawn;
    }
    t += 1000 / kFramesPerSecond;
  }

  const std::vector<float> halo = blur(img.px, img.w, img.h, 5);
  for (size_t i = 0; i < img.px.size(); ++i) img.px[i] += halo[i] * kGlow;

  // Paint the unlit surround last so the glow does not wash it out.
  for (int y = 0; y < img.h; ++y) {
    for (int x = 0; x < img.w; ++x) {
      const size_t i = (static_cast<size_t>(y) * img.w + x) * 3;
      img.px[i] += 11;
      img.px[i + 1] += 13;
      img.px[i + 2] += 16;
    }
  }

  const std::string path = dir + "/" + pattern_name(pat) + ".ppm";
  if (write_ppm(path, img)) {
    std::printf("  %s\n", path.c_str());
  } else {
    std::printf("  FAILED to write %s\n", path.c_str());
  }
}

// Each screen sheet shows four states of that screen rather than four moments
// in time, so one image says what the screen is for.
void vary_state(Screen s, int frame, UiState& ui) {
  switch (s) {
    case Screen::Status:
      ui.status = static_cast<Status>(frame % static_cast<int>(Status::Count));
      break;
    case Screen::Clock:
      ui.hour = 9 + frame * 2;
      ui.minute = 5 + frame * 17;
      ui.second = frame * 14;
      break;
    case Screen::Timer: {
      const int left[] = {25 * 60, 12 * 60 + 30, 60 + 5, 45};
      ui.timer_left_s = left[frame % 4];
      ui.timer_running = frame != 3;  // the last one is paused, so it blinks
      break;
    }
    case Screen::Brightness: {
      const uint8_t b[] = {26, 89, 178, 255};
      ui.brightness = b[frame % 4];
      break;
    }
    case Screen::ColorPick:
      ui.hue = frame * 0.25f + 0.04f;
      break;
    case Screen::TimerSet: {
      const int m[] = {5, 25, 45, 60};
      ui.timer_set_min = m[frame % 4];
      break;
    }
    case Screen::Booting:
      ui.wifi_connected = (frame == 3);
      break;
    default:
      break;
  }
}

void render_screen_sheet(Screen s, const std::string& dir) {
  const int fw = kWidth * kPitch + 2 * kMargin;
  const int fh = kHeight * kPitch + 2 * kMargin;
  Image img(fw, kFramesPerSheet * fh + (kFramesPerSheet - 1) * kFrameGap);

  Framebuffer fb;
  uint8_t grb[kNumLeds * 3];
  for (int frame = 0; frame < kFramesPerSheet; ++frame) {
    UiState ui;
    ui.accent = RGB(255, 138, 31);
    ui.hour = 14;
    ui.minute = 25;
    ui.second = 37;
    vary_state(s, frame, ui);
    // Offset the clock per frame so pulses and blinks land in different phases.
    const uint32_t now = 300u + frame * 700u;
    draw_screen(fb, s, ui, now);
    fb.render(grb, kPreviewBrightness, kMaxMilliamps, kWiring);
    draw_frame(img, grb, kMargin, frame * (fh + kFrameGap) + kMargin);
  }

  const std::vector<float> halo = blur(img.px, img.w, img.h, 5);
  for (size_t i = 0; i < img.px.size(); ++i) img.px[i] += halo[i] * kGlow;
  for (int y = 0; y < img.h; ++y) {
    for (int x = 0; x < img.w; ++x) {
      const size_t i = (static_cast<size_t>(y) * img.w + x) * 3;
      img.px[i] += 11;
      img.px[i + 1] += 13;
      img.px[i + 2] += 16;
    }
  }

  const std::string path = dir + "/" + screen_name(s) + ".ppm";
  if (write_ppm(path, img)) {
    std::printf("  %s\n", path.c_str());
  } else {
    std::printf("  FAILED to write %s\n", path.c_str());
  }
}

// ---------------------------------------------------------------- animation
//
// Writes a clip as concatenated P6 frames in one file, with the frame rate in
// a leading comment. Still images cannot show whether motion is smooth, and
// that is the whole point of this work.

constexpr int kAnimFps = 25;          // 40 ms, which GIF can express exactly
constexpr int kAnimPitch = 8;         // smaller cells: these files go in git
constexpr int kAnimCell = 7;

void draw_frame_small(Image& img, const uint8_t* grb, int ox, int oy) {
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      const int i = led_index(x, y, kWiring) * 3;
      const float g = grb[i + 0], r = grb[i + 1], b = grb[i + 2];
      for (int dy = 0; dy < kAnimCell; ++dy)
        for (int dx = 0; dx < kAnimCell; ++dx)
          img.set(ox + x * kAnimPitch + dx, oy + y * kAnimPitch + dy, r, g, b);
    }
  }
}

// What a clip does on each frame. Returns the screen to show.
struct Clip {
  const char* name;
  Screen screen;
  Status status;
  float seconds;
  // Optional screen change part way through, to capture a transition.
  Screen go_to = Screen::Count;
  float go_at = -1.0f;
  TransitionKind kind = TransitionKind::None;
  // For a dissolve, the status the panel changes to at the same moment.
  Status status_after = Status::Count;
  // Walk a list screen, one entry every `scroll_every` seconds, with the disk
  // transition the real thing uses.
  float scroll_every = 0.0f;
  // Fire the completion flourish at this moment, over whatever is showing.
  float done_at = -1.0f;
};

void render_clip(const Clip& c, const std::string& dir) {
  const int fw = kWidth * kAnimPitch + 2 * kMargin;
  const int fh = kHeight * kAnimPitch + 2 * kMargin;
  const int frames = static_cast<int>(c.seconds * kAnimFps);

  ScreenManager mgr;
  mgr.set_screen(c.screen);
  UiState ui;
  ui.accent = RGB(255, 138, 31);
  ui.status = c.status;
  ui.hour = 14;
  ui.minute = 25;
  ui.brightness = 178;
  ui.timer_total_s = 25 * 60;
  ui.timer_left_s = 25 * 60;
  ui.timer_running = true;
  ui.timer_set_min = 25;
  ui.hue = 0.08f;

  Framebuffer fb;
  Renderer ren;
  uint8_t grb[kNumLeds * 3];
  const std::string path = dir + "/" + c.name + ".ppms";
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) {
    std::printf("  FAILED to open %s\n", path.c_str());
    return;
  }

  FrameClock clock;
  micros_t t = 0;
  bool fired = false;
  bool done_fired = false;
  Flourish fl;
  float next_scroll = c.scroll_every;
  for (int i = 0; i < frames; ++i) {
    const float secs = static_cast<float>(i) / kAnimFps;
    if (!fired && c.go_at >= 0.0f && secs >= c.go_at) {
      fired = true;
      // Tell the manager what it is leaving before the state changes, so the
      // outgoing side is drawn with the state it actually had.
      if (c.kind == TransitionKind::None) {
        mgr.go_to(c.go_to, ui);
      } else if (c.go_to == mgr.current()) {
        // A change in place, not a new screen.
        mgr.restart_with(ui, c.kind, transition_seconds(c.kind));
      } else {
        mgr.go_to(c.go_to, ui, c.kind);
      }
      if (c.status_after != Status::Count) ui.status = c.status_after;
    }
    // Walk a list, exactly as the model does: change the index only after the
    // manager has been told what it is leaving.
    if (c.scroll_every > 0.0f && secs >= next_scroll) {
      next_scroll += c.scroll_every;
      mgr.restart_with(ui, TransitionKind::DiskUp,
                       transition_seconds(TransitionKind::DiskUp));
      if (c.screen == Screen::Menu) {
        ui.menu_index = static_cast<uint8_t>((ui.menu_index + 1) % kMenuCount);
      } else {
        ui.pick = static_cast<Status>((static_cast<int>(ui.pick) + 1) %
                                      static_cast<int>(Status::Count));
      }
    }

    ui.boot_t = secs;  // the boot sequence runs on its own clock, once

    // A live second hand and a counting timer, so the clips show real motion.
    ui.second = static_cast<int>(secs) % 60;
    ui.timer_left_s = 25 * 60 - static_cast<int>(secs * 30.0f);
    ui.hue = 0.08f + secs * 0.05f;
    if (ui.hue > 1.0f) ui.hue -= 1.0f;

    const Anim a = clock.tick(t);
    if (!done_fired && c.done_at >= 0.0f && secs >= c.done_at) {
      done_fired = true;
      fl.done(status_color(ui.status), "DONE", a.t);
    }
    mgr.advance(a.dt);
    fl.tick(a.t);
    if (fl.opaque(a.t)) {
      fb.clear();
    } else {
      mgr.render(fb, ui, a);
    }
    fl.draw(fb, a);
    ren.render(fb, grb, kPreviewBrightness, kMaxMilliamps, kWiring);

    Image img(fw, fh);
    draw_frame_small(img, grb, kMargin, kMargin);
    const std::vector<float> halo = blur(img.px, img.w, img.h, 3);
    for (size_t k = 0; k < img.px.size(); ++k) img.px[k] += halo[k] * kGlow;
    for (int y = 0; y < img.h; ++y)
      for (int x = 0; x < img.w; ++x) {
        const size_t k = (static_cast<size_t>(y) * img.w + x) * 3;
        img.px[k] += 11; img.px[k + 1] += 13; img.px[k + 2] += 16;
      }

    if (i == 0) std::fprintf(f, "# fps %d\n", kAnimFps);
    std::fprintf(f, "P6\n%d %d\n255\n", img.w, img.h);
    std::vector<uint8_t> row(static_cast<size_t>(img.w) * 3);
    for (int y = 0; y < img.h; ++y) {
      for (int x = 0; x < img.w * 3; ++x) {
        float v = img.px[(static_cast<size_t>(y) * img.w * 3) + x];
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        row[x] = static_cast<uint8_t>(v + 0.5f);
      }
      std::fwrite(row.data(), 1, row.size(), f);
    }
    t += 1000000 / kAnimFps;
  }
  std::fclose(f);
  std::printf("  %s  %d frames\n", path.c_str(), frames);
}

void render_all_clips(const std::string& dir) {
  const Clip clips[] = {
      {"status-free", Screen::Status, Status::Free, 3.0f},
      {"status-busy", Screen::Status, Status::Busy, 3.0f},
      {"status-call", Screen::Status, Status::Call, 3.0f},
      {"status-dnd", Screen::Status, Status::Dnd, 3.0f},
      // A badge status: the word too wide for the label box, knocked out of a
      // full-width field of its own colour.
      {"status-focus", Screen::Status, Status::Focus, 3.0f},
      {"status-lunch", Screen::Status, Status::Lunch, 3.0f},
      {"menu", Screen::Menu, Status::Free, 6.0f, Screen::Count, -1.0f,
       TransitionKind::None, Status::Count, 1.2f},
      {"statuspick", Screen::StatusPick, Status::Free, 8.0f, Screen::Count, -1.0f,
       TransitionKind::None, Status::Count, 1.0f},
      {"clock", Screen::Clock, Status::Free, 4.0f},
      {"timer", Screen::Timer, Status::Busy, 4.0f},
      {"colorpick", Screen::ColorPick, Status::Free, 4.0f},
      {"sleep", Screen::Sleep, Status::Free, 4.0f},
      {"booting", Screen::Booting, Status::Free, 2.2f},
      {"trans-disk", Screen::Status, Status::Busy, 2.2f, Screen::Clock, 0.6f},
      // The timer finishing: a flash, a field, the word held, then a fade that
      // uncovers the screen rather than painting over it.
      {"flourish-done", Screen::Timer, Status::Busy, 3.2f, Screen::Count, -1.0f,
       TransitionKind::None, Status::Count, 0.0f, 0.7f},
      {"trans-wipe", Screen::Timer, Status::Busy, 2.0f, Screen::Brightness, 0.6f},
      {"trans-fade", Screen::Status, Status::Busy, 2.5f, Screen::Sleep, 0.7f},
      // A status change with no screen transition at all: the icon and the word
      // turn over where they stand. This is the one to watch to judge whether
      // the motion reads as an object changing or as pixels being swapped.
      {"swap-status", Screen::Status, Status::Free, 2.4f, Screen::Status, 0.7f,
       TransitionKind::None, Status::Busy},
      // Claiming a status: the new colour bursts out of the icon, takes the
      // panel, and is drawn back in leaving the new word behind.
      {"trans-ignite", Screen::Status, Status::Free, 2.6f, Screen::Status, 0.6f,
       TransitionKind::Ignite, Status::Busy},
      {"trans-ignite-dnd", Screen::Status, Status::Busy, 2.6f, Screen::Status, 0.6f,
       TransitionKind::Ignite, Status::Dnd},
  };
  for (const Clip& c : clips) render_clip(c, dir);
}

}  // namespace

int main(int argc, char** argv) {
  // --anim writes animated clips instead of still sheets.
  if (argc > 1 && std::string(argv[1]) == "--anim") {
    const std::string dir = (argc > 2) ? argv[2] : "docs/anim";
    std::printf("clips into %s\n", dir.c_str());
    render_all_clips(dir);
    return 0;
  }
  const std::string pdir = (argc > 1) ? argv[1] : "docs/preview";
  const std::string sdir = (argc > 2) ? argv[2] : "docs/screens";
  std::printf("patterns into %s\n", pdir.c_str());
  for (int i = 0; i < static_cast<int>(Pattern::Count); ++i) {
    render_sheet(static_cast<Pattern>(i), pdir);
  }
  std::printf("screens into %s\n", sdir.c_str());
  for (int i = 0; i < static_cast<int>(Screen::Count); ++i) {
    render_screen_sheet(static_cast<Screen>(i), sdir);
  }
  return 0;
}
