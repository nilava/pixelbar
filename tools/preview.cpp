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
#include "panel/framebuffer.h"
#include "panel/patterns.h"

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

}  // namespace

int main(int argc, char** argv) {
  const std::string dir = (argc > 1) ? argv[1] : ".";
  std::printf("rendering previews into %s\n", dir.c_str());
  for (int i = 0; i < static_cast<int>(Pattern::Count); ++i) {
    render_sheet(static_cast<Pattern>(i), dir);
  }
  return 0;
}
