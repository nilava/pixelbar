#include "panel/patterns.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "panel/font.h"

namespace panel {
namespace {

constexpr float kScrollPxPerSec = 12.0f;
constexpr float kSparkDecayPerSec = 1.8f;
constexpr float kSparkSpawnPerSec = 14.0f;
constexpr float kMapTestPxPerSec = 8.0f;

char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool ieq(const char* a, const char* b) {
  for (; *a && *b; ++a, ++b) {
    if (lower(*a) != lower(*b)) return false;
  }
  return *a == *b;
}

}  // namespace

const char* pattern_name(Pattern p) {
  switch (p) {
    case Pattern::Text: return "text";
    case Pattern::Clock: return "clock";
    case Pattern::Rainbow: return "rainbow";
    case Pattern::Plasma: return "plasma";
    case Pattern::Sparkle: return "sparkle";
    case Pattern::Solid: return "solid";
    case Pattern::MapTest: return "maptest";
    default: return "?";
  }
}

bool pattern_from_name(const char* name, Pattern* out) {
  if (!name || !out) return false;
  for (int i = 0; i < static_cast<int>(Pattern::Count); ++i) {
    const Pattern p = static_cast<Pattern>(i);
    if (ieq(name, pattern_name(p))) {
      *out = p;
      return true;
    }
  }
  return false;
}

Engine::Engine() { std::snprintf(params.text, kMaxText, "PIXELBAR"); }

void Engine::set_pattern(Pattern p) {
  if (p == pat_ || p >= Pattern::Count) return;
  pat_ = p;
  scroll_ = 0.0f;
}

void Engine::set_text(const char* s) {
  if (!s) return;
  std::snprintf(params.text, kMaxText, "%s", s);
  scroll_ = 0.0f;
}

void Engine::render(Framebuffer& fb, uint32_t now_ms) {
  float dt = 0.0f;
  if (!started_) {
    started_ = true;
  } else {
    // Unsigned subtraction handles the 49-day wrap of a millisecond counter.
    const uint32_t elapsed = now_ms - last_ms_;
    dt = static_cast<float>(elapsed) / 1000.0f;
    if (dt > 0.25f) dt = 0.25f;  // a long stall should not jump the animation
  }
  last_ms_ = now_ms;
  const float sdt = dt * params.speed;
  t_ += sdt;
  scroll_ += sdt * kScrollPxPerSec;

  fb.clear();
  switch (pat_) {
    case Pattern::Text: draw_text(fb); break;
    case Pattern::Clock: draw_clock(fb); break;
    case Pattern::Rainbow: draw_rainbow(fb); break;
    case Pattern::Plasma: draw_plasma(fb); break;
    case Pattern::Sparkle: draw_sparkle(fb, sdt); break;
    case Pattern::Solid: fb.fill(params.color); break;
    case Pattern::MapTest: draw_maptest(fb); break;
    default: break;
  }
}

void Engine::draw_text(Framebuffer& fb) {
  const int textW = measure_text(params.text);
  const int total = textW + kWidth;  // scroll fully off before repeating
  if (total <= 0) return;
  while (scroll_ >= static_cast<float>(total)) scroll_ -= static_cast<float>(total);
  const int x = kWidth - static_cast<int>(scroll_);
  panel::draw_text(fb, x, 0, params.text, params.color);
}

void Engine::draw_clock(Framebuffer& fb) {
  // Normalise into range by hand rather than formatting: it cannot overflow,
  // and it keeps a negative or out-of-range input from drawing nonsense.
  const int h = ((params.hour % 24) + 24) % 24;
  const int m = ((params.minute % 60) + 60) % 60;
  const char hh[2] = {static_cast<char>('0' + h / 10), static_cast<char>('0' + h % 10)};
  const char mm[2] = {static_cast<char>('0' + m / 10), static_cast<char>('0' + m % 10)};

  // 4 digits at 4 px each plus a 2 px colon = 18 px, centred in 24.
  int x = 3;
  const int y = 1;
  draw_tiny_digit(fb, x, y, hh[0], params.color); x += 4;
  draw_tiny_digit(fb, x, y, hh[1], params.color); x += 4;
  if (params.second % 2 == 0) {
    fb.set(x, y + 1, params.color);
    fb.set(x, y + 3, params.color);
  }
  x += 2;
  draw_tiny_digit(fb, x, y, mm[0], params.color); x += 4;
  draw_tiny_digit(fb, x, y, mm[1], params.color);

  // Seconds as a progress bar along the bottom row.
  const RGB dim = params.color.scaled(90);
  const int lit = (params.second * kWidth) / 60;
  for (int i = 0; i < lit; ++i) fb.set(i, kHeight - 1, dim);
}

void Engine::draw_rainbow(Framebuffer& fb) {
  for (int x = 0; x < kWidth; ++x) {
    for (int y = 0; y < kHeight; ++y) {
      fb.set(x, y, hsv(x * 0.04f + y * 0.02f + t_ * 0.25f, 1.0f, 1.0f));
    }
  }
}

void Engine::draw_plasma(Framebuffer& fb) {
  for (int x = 0; x < kWidth; ++x) {
    for (int y = 0; y < kHeight; ++y) {
      const float v = std::sin(x * 0.35f + t_ * 2.0f) +
                      std::sin(y * 0.60f - t_ * 1.3f) +
                      std::sin((x + y) * 0.30f + t_);
      const float val = 0.5f + 0.5f * std::sin(v * 1.5f);
      fb.set(x, y, hsv(v / 6.0f + 0.5f + t_ * 0.05f, 0.9f, val));
    }
  }
}

void Engine::draw_sparkle(Framebuffer& fb, float dt) {
  for (int i = 0; i < kNumLeds; ++i) {
    sparks_[i] -= dt * kSparkDecayPerSec;
    if (sparks_[i] < 0.0f) sparks_[i] = 0.0f;
  }
  // Spawn with the right long-run rate even when dt varies.
  float chance = dt * kSparkSpawnPerSec;
  while (chance > 0.0f) {
    if (chance >= 1.0f || rng_.unit() < chance) {
      sparks_[rng_.next() % kNumLeds] = 1.0f;
    }
    chance -= 1.0f;
  }
  for (int x = 0; x < kWidth; ++x) {
    for (int y = 0; y < kHeight; ++y) {
      const float v = sparks_[y * kWidth + x];
      if (v > 0.0f) {
        fb.set(x, y, params.color.scaled(static_cast<uint8_t>(v * 255.0f + 0.5f)));
      }
    }
  }
}

void Engine::draw_maptest(Framebuffer& fb) {
  // A white pixel walks logical order: x fastest, then y. With the Wiring
  // config correct it sweeps left to right along the top row first.
  const int n = static_cast<int>(t_ * kMapTestPxPerSec) % kNumLeds;
  // Dim trail behind the head makes the direction obvious.
  for (int k = 3; k >= 1; --k) {
    const int p = (n - k + kNumLeds) % kNumLeds;
    fb.set(p % kWidth, p / kWidth, RGB(60 / k, 60 / k, 60 / k));
  }
  // Tile seams: logical columns 8 and 16 marked green on the bottom row.
  fb.set(kTileW, kHeight - 1, RGB(0, 120, 0));
  fb.set(kTileW * 2, kHeight - 1, RGB(0, 120, 0));
  // Origin marker: logical (0,0) is red, so you can tell which corner is which.
  fb.set(0, 0, RGB(255, 0, 0));
  // The head goes on last: it has to stay visible even where it crosses the
  // origin marker or a seam marker, since it is the thing being tracked.
  fb.set(n % kWidth, n / kWidth, RGB(255, 255, 255));
}

}  // namespace panel
