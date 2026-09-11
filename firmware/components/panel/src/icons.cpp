#include "panel/icons.h"

#include <cmath>

#include "panel/anim.h"
#include "panel/framebuffer.h"

namespace panel {
namespace {

// Eight binary digits per row, so the shape is readable in the source.
#define IR(a, b, c, d, e, f, g, h)                                            \
  static_cast<uint8_t>((a) << 7 | (b) << 6 | (c) << 5 | (d) << 4 | (e) << 3 | \
                       (f) << 2 | (g) << 1 | (h))

}  // namespace

const Icon kIconFree = {{
    IR(0,0,1,1,1,1,0,0),
    IR(0,1,0,0,0,0,1,0),
    IR(1,0,0,0,0,0,0,1),
    IR(1,0,0,0,0,0,0,1),
    IR(1,0,0,0,0,0,0,1),
    IR(1,0,0,0,0,0,0,1),
    IR(0,1,0,0,0,0,1,0),
    IR(0,0,1,1,1,1,0,0),
}};

const Icon kIconBusy = {{
    IR(0,0,1,1,1,1,0,0),
    IR(0,1,1,1,1,1,1,0),
    IR(1,1,1,1,1,1,1,1),
    IR(1,1,1,1,1,1,1,1),
    IR(1,1,1,1,1,1,1,1),
    IR(1,1,1,1,1,1,1,1),
    IR(0,1,1,1,1,1,1,0),
    IR(0,0,1,1,1,1,0,0),
}};

const Icon kIconDnd = {{
    IR(0,0,1,1,1,1,0,0),
    IR(0,1,0,0,0,0,1,0),
    IR(1,0,0,0,0,1,0,1),
    IR(1,0,0,0,1,0,0,1),
    IR(1,0,0,1,0,0,0,1),
    IR(1,0,1,0,0,0,0,1),
    IR(0,1,0,0,0,0,1,0),
    IR(0,0,1,1,1,1,0,0),
}};

namespace {
// The handset never touches rows 1 and 2, which is where the ripples live.
const Icon kCallFrames[4] = {
    {{IR(0,0,0,0,0,0,0,0), IR(0,0,0,0,0,0,0,0), IR(0,0,0,0,0,0,0,0),
      IR(1,1,0,0,0,0,1,1), IR(1,1,0,0,0,0,1,1), IR(0,1,0,0,0,0,1,0),
      IR(0,0,1,1,1,1,0,0), IR(0,0,0,0,0,0,0,0)}},
    {{IR(0,0,0,0,0,0,0,0), IR(0,0,0,0,0,0,0,0), IR(0,1,0,0,0,0,1,0),
      IR(1,1,0,0,0,0,1,1), IR(1,1,0,0,0,0,1,1), IR(0,1,0,0,0,0,1,0),
      IR(0,0,1,1,1,1,0,0), IR(0,0,0,0,0,0,0,0)}},
    {{IR(0,0,0,0,0,0,0,0), IR(1,0,0,0,0,0,0,1), IR(0,1,0,0,0,0,1,0),
      IR(1,1,0,0,0,0,1,1), IR(1,1,0,0,0,0,1,1), IR(0,1,0,0,0,0,1,0),
      IR(0,0,1,1,1,1,0,0), IR(0,0,0,0,0,0,0,0)}},
    {{IR(0,0,0,0,0,0,0,0), IR(1,0,0,0,0,0,0,1), IR(0,0,0,0,0,0,0,0),
      IR(1,1,0,0,0,0,1,1), IR(1,1,0,0,0,0,1,1), IR(0,1,0,0,0,0,1,0),
      IR(0,0,1,1,1,1,0,0), IR(0,0,0,0,0,0,0,0)}},
};
}  // namespace
const AnimIcon kAnimCall = {kCallFrames, 4, 8.0f, false};

const Icon kIconMoon = {{
    IR(0,0,0,1,1,1,0,0),
    IR(0,0,1,1,0,0,0,0),
    IR(0,1,1,0,0,0,0,0),
    IR(0,1,1,0,0,0,0,1),
    IR(0,1,1,0,0,0,0,0),
    IR(0,1,1,0,0,0,0,0),
    IR(0,0,1,1,0,0,0,0),
    IR(0,0,0,1,1,1,0,0),
}};

const Icon kIconSunCore = {{
    IR(0,0,0,0,0,0,0,0),
    IR(0,0,0,0,0,0,0,0),
    IR(0,0,1,1,1,1,0,0),
    IR(0,0,1,1,1,1,0,0),
    IR(0,0,1,1,1,1,0,0),
    IR(0,0,1,1,1,1,0,0),
    IR(0,0,0,0,0,0,0,0),
    IR(0,0,0,0,0,0,0,0),
}};

const Icon kIconSunRays = {{
    IR(0,0,0,1,1,0,0,0),
    IR(0,1,0,0,0,0,1,0),
    IR(0,0,0,0,0,0,0,0),
    IR(1,0,0,0,0,0,0,1),
    IR(1,0,0,0,0,0,0,1),
    IR(0,0,0,0,0,0,0,0),
    IR(0,1,0,0,0,0,1,0),
    IR(0,0,0,1,1,0,0,0),
}};

const Icon kIconHourglass = {{
    IR(0,1,1,1,1,1,1,0),
    IR(0,1,0,0,0,0,1,0),
    IR(0,0,1,0,0,1,0,0),
    IR(0,0,0,1,1,0,0,0),
    IR(0,0,1,0,0,1,0,0),
    IR(0,1,0,0,0,0,1,0),
    IR(0,1,0,0,0,0,1,0),
    IR(0,1,1,1,1,1,1,0),
}};

const Icon kIconHourglassTop = {{
    IR(0,0,0,0,0,0,0,0),
    IR(0,0,1,1,1,1,0,0),
    IR(0,0,0,1,1,0,0,0),
    IR(0,0,0,0,0,0,0,0),
    IR(0,0,0,0,0,0,0,0),
    IR(0,0,0,0,0,0,0,0),
    IR(0,0,0,0,0,0,0,0),
    IR(0,0,0,0,0,0,0,0),
}};

const Icon kIconHourglassBottom = {{
    IR(0,0,0,0,0,0,0,0),
    IR(0,0,0,0,0,0,0,0),
    IR(0,0,0,0,0,0,0,0),
    IR(0,0,0,0,0,0,0,0),
    IR(0,0,0,1,1,0,0,0),
    IR(0,0,1,1,1,1,0,0),
    IR(0,1,1,1,1,1,1,0),
    IR(0,0,0,0,0,0,0,0),
}};

namespace {
// Each frame lights one more arc than the last, so it reads as filling.
const Icon kWifiFrames[5] = {
    {{IR(0,0,0,0,0,0,0,0), IR(0,0,0,0,0,0,0,0), IR(0,0,0,0,0,0,0,0),
      IR(0,0,0,0,0,0,0,0), IR(0,0,0,0,0,0,0,0), IR(0,0,0,0,0,0,0,0),
      IR(0,0,0,1,1,0,0,0), IR(0,0,0,1,1,0,0,0)}},
    {{IR(0,0,0,0,0,0,0,0), IR(0,0,0,0,0,0,0,0), IR(0,0,0,0,0,0,0,0),
      IR(0,0,0,0,0,0,0,0), IR(0,0,0,1,1,0,0,0), IR(0,0,0,0,0,0,0,0),
      IR(0,0,0,1,1,0,0,0), IR(0,0,0,1,1,0,0,0)}},
    {{IR(0,0,0,0,0,0,0,0), IR(0,0,0,0,0,0,0,0), IR(0,0,1,1,1,1,0,0),
      IR(0,1,0,0,0,0,1,0), IR(0,0,0,1,1,0,0,0), IR(0,0,0,0,0,0,0,0),
      IR(0,0,0,1,1,0,0,0), IR(0,0,0,1,1,0,0,0)}},
    {{IR(0,1,1,1,1,1,1,0), IR(1,0,0,0,0,0,0,1), IR(0,0,1,1,1,1,0,0),
      IR(0,1,0,0,0,0,1,0), IR(0,0,0,1,1,0,0,0), IR(0,0,0,0,0,0,0,0),
      IR(0,0,0,1,1,0,0,0), IR(0,0,0,1,1,0,0,0)}},
    {{IR(0,1,1,1,1,1,1,0), IR(1,0,0,0,0,0,0,1), IR(0,0,1,1,1,1,0,0),
      IR(0,1,0,0,0,0,1,0), IR(0,0,0,1,1,0,0,0), IR(0,0,0,0,0,0,0,0),
      IR(0,0,0,1,1,0,0,0), IR(0,0,0,1,1,0,0,0)}},
};
// Short arm first, then up the long one, so it draws like a pen stroke.
const uint8_t kCheckPts[] = {0x13, 0x24, 0x35, 0x44, 0x53, 0x62, 0x71};
}  // namespace
const AnimIcon kAnimWifi = {kWifiFrames, 5, 4.0f, false};
const StrokeIcon kStrokeCheck = {kCheckPts, sizeof(kCheckPts)};

#undef IR

// ---------------------------------------------------------------- drawing

void draw_icon(Framebuffer& fb, int x, int y, const Icon& ic, RGB color) {
  for (int row = 0; row < kIconH; ++row) {
    for (int col = 0; col < kIconW; ++col) {
      if (ic.rows[row] & (1 << (kIconW - 1 - col))) fb.set(x + col, y + row, color);
    }
  }
}

void draw_icon_aa(Framebuffer& fb, float x, float y, const Icon& ic, RGB color) {
  for (int row = 0; row < kIconH; ++row) {
    for (int col = 0; col < kIconW; ++col) {
      if (ic.rows[row] & (1 << (kIconW - 1 - col))) {
        fb.set_aa2(x + col, y + row, color, 1.0f, Blend::Add);
      }
    }
  }
}

int icon_lit_count(const Icon& ic) {
  int n = 0;
  for (int row = 0; row < kIconH; ++row) {
    for (int col = 0; col < kIconW; ++col) {
      if (ic.rows[row] & (1 << (kIconW - 1 - col))) ++n;
    }
  }
  return n;
}

const Icon& anim_frame(const AnimIcon& a, double t_s) {
  if (a.count == 0) return a.frames[0];
  double steps = t_s * a.fps;
  if (steps < 0) steps = 0;
  long i = static_cast<long>(steps);
  if (a.ping_pong && a.count > 1) {
    const long span = 2 * a.count - 2;
    long p = i % span;
    if (p < 0) p += span;
    if (p >= a.count) p = span - p;
    return a.frames[p];
  }
  return a.frames[i % a.count];
}

void draw_anim_icon(Framebuffer& fb, int x, int y, const AnimIcon& a, double t_s, RGB color) {
  draw_icon(fb, x, y, anim_frame(a, t_s), color);
}

void draw_icon_masked(Framebuffer& fb, int x, int y, const Icon& shell, const Icon& fill,
                      float fraction, RGB shell_c, RGB fill_c) {
  draw_icon(fb, x, y, shell, shell_c);
  const float level = clamp01(fraction) * kIconH;  // rows lit from the bottom
  for (int row = 0; row < kIconH; ++row) {
    const int from_bottom = kIconH - 1 - row;
    float cov = level - from_bottom;
    if (cov <= 0.0f) continue;
    if (cov > 1.0f) cov = 1.0f;
    for (int col = 0; col < kIconW; ++col) {
      if (fill.rows[row] & (1 << (kIconW - 1 - col))) {
        fb.blend(x + col, y + row, fill_c, cov);
      }
    }
  }
}

void draw_stroke_icon(Framebuffer& fb, int x, int y, const StrokeIcon& s, float progress,
                      RGB body, RGB tip) {
  const float p = clamp01(progress) * s.n;
  const int full = static_cast<int>(p);
  for (int i = 0; i < s.n && i <= full; ++i) {
    const int px = (s.pts[i] >> 4) & 0xF;
    const int py = s.pts[i] & 0xF;
    if (i == full) {
      fb.blend(x + px, y + py, tip, p - full);  // the leading point fades in
    } else {
      fb.set(x + px, y + py, body);
    }
  }
}

void draw_hourglass(Framebuffer& fb, int x, int y, float remaining, double t_s,
                    RGB shell, RGB sand) {
  draw_icon(fb, x, y, kIconHourglass, shell);
  // Sand left in the top chamber, sand collected in the bottom.
  const float r = clamp01(remaining);
  for (int row = 0; row < kIconH; ++row) {
    for (int col = 0; col < kIconW; ++col) {
      if (kIconHourglassTop.rows[row] & (1 << (kIconW - 1 - col))) {
        fb.blend(x + col, y + row, sand, r);
      }
      if (kIconHourglassBottom.rows[row] & (1 << (kIconW - 1 - col))) {
        fb.blend(x + col, y + row, sand, 1.0f - r);
      }
    }
  }
  // One grain in transit, so it reads as running rather than paused.
  if (r > 0.0f && r < 1.0f) {
    const int gx = (static_cast<int>(t_s * 2.0) & 1) ? 4 : 3;
    fb.set(x + gx, y + 4, sand);
  }
}

}  // namespace panel
