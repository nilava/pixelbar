#include "panel/sprite.h"

#include <cmath>

#include "panel/anim.h"
#include "panel/icons.h"
#include "panel/mini_font.h"

namespace panel {

Sprite sprite_of(const Icon& ic) { return Sprite{ic.rows, kIconW, kIconH, 7}; }

Sprite sprite_of(const MiniGlyph& g) { return Sprite{g.rows, g.w, kMiniH, 4}; }

namespace {

// Overlap of [a0,a1) with [b0,b1).
inline float overlap(float a0, float a1, float b0, float b1) {
  const float lo = a0 > b0 ? a0 : b0;
  const float hi = a1 < b1 ? a1 : b1;
  return hi > lo ? hi - lo : 0.0f;
}

inline int floor_i(float v) { return static_cast<int>(std::floor(v)); }

}  // namespace

void draw_sprite_scaled(Framebuffer& fb, const Sprite& s, float cx, float cy,
                        float sx, float sy, RGB color, float bright, int clip_y0,
                        int clip_y1, Ink ink, Blend b) {
  if (!s.rows || s.w == 0 || s.h == 0) return;
  if (bright <= 0.0f) return;
  if (sx == 0.0f || sy == 0.0f) return;

  // Each source texel maps to an axis-aligned rectangle in panel space. Splat
  // it by area: the overlap with each destination pixel is exactly the
  // coverage that pixel should receive. This is forward mapping rather than
  // inverse because it stays correct when the rectangle is smaller than one
  // pixel, which is the whole point when the scale approaches zero.
  const float half_w = s.w * 0.5f;
  const float half_h = s.h * 0.5f;

  // Normalised keeps the per-area emission constant, so shrinking a sprite
  // must not concentrate its ink. The area of one texel is |sx*sy|; dividing
  // by it would conserve, so we simply do not, and instead cap the coverage
  // any one destination pixel can receive from a single texel at 1.
  const float texel_area = std::fabs(sx) * std::fabs(sy);
  const float gain = (ink == Ink::Conserved || texel_area <= 0.0f)
                         ? 1.0f
                         : (1.0f / texel_area);

  if (clip_y0 < 0) clip_y0 = 0;
  if (clip_y1 > kHeight) clip_y1 = kHeight;

  for (int j = 0; j < s.h; ++j) {
    float y0 = cy + (static_cast<float>(j) - half_h) * sy;
    float y1 = cy + (static_cast<float>(j) + 1.0f - half_h) * sy;
    if (y1 < y0) {
      const float t = y0;
      y0 = y1;
      y1 = t;
    }
    const int jy0 = floor_i(y0);
    const int jy1 = floor_i(y1 - 1e-6f);

    for (int i = 0; i < s.w; ++i) {
      if (!s.on(i, j)) continue;
      float x0 = cx + (static_cast<float>(i) - half_w) * sx;
      float x1 = cx + (static_cast<float>(i) + 1.0f - half_w) * sx;
      if (x1 < x0) {
        const float t = x0;
        x0 = x1;
        x1 = t;
      }
      const int ix0 = floor_i(x0);
      const int ix1 = floor_i(x1 - 1e-6f);

      for (int py = jy0; py <= jy1; ++py) {
        if (py < clip_y0 || py >= clip_y1) continue;
        const float cov_y = overlap(y0, y1, static_cast<float>(py),
                                    static_cast<float>(py) + 1.0f);
        if (cov_y <= 0.0f) continue;
        for (int px = ix0; px <= ix1; ++px) {
          if (px < 0 || px >= kWidth) continue;
          const float cov_x = overlap(x0, x1, static_cast<float>(px),
                                      static_cast<float>(px) + 1.0f);
          if (cov_x <= 0.0f) continue;
          float cov = cov_x * cov_y * gain * bright;
          if (cov > bright) cov = bright;  // one texel never over-lights a pixel
          if (cov <= 0.0f) continue;
          if (b == Blend::Add) {
            fb.add_scaled(px, py, color, cov);
          } else {
            fb.blend(px, py, color, cov);
          }
        }
      }
    }
  }
}

void draw_sprite_rotated(Framebuffer& fb, const Sprite& s, float cx, float cy,
                         float turns, RGB color, float bright, float scale,
                         Blend b, int clip_x0, int clip_x1) {
  if (!s.rows || s.w == 0 || s.h == 0 || bright <= 0.0f) return;
  if (scale <= 0.0f) return;

  const float c = fast_cos(turns);
  const float sn = fast_sin(turns);
  const float inv = 1.0f / scale;
  const float half_w = s.w * 0.5f;
  const float half_h = s.h * 0.5f;

  // The rotated sprite fits inside a disc of the diagonal's radius.
  const float radius = 0.5f * std::sqrt(static_cast<float>(s.w * s.w + s.h * s.h)) * scale + 1.0f;
  const int x0 = floor_i(cx - radius), x1 = floor_i(cx + radius);
  const int y0 = floor_i(cy - radius), y1 = floor_i(cy + radius);

  if (clip_x0 < 0) clip_x0 = 0;
  if (clip_x1 > kWidth) clip_x1 = kWidth;

  for (int py = y0; py <= y1; ++py) {
    if (py < 0 || py >= kHeight) continue;
    for (int px = x0; px <= x1; ++px) {
      if (px < clip_x0 || px >= clip_x1) continue;
      // Inverse-map the pixel centre into source space.
      const float dx = (static_cast<float>(px) + 0.5f - cx) * inv;
      const float dy = (static_cast<float>(py) + 0.5f - cy) * inv;
      const float u = (dx * c + dy * sn) + half_w - 0.5f;
      const float v = (-dx * sn + dy * c) + half_h - 0.5f;

      // Bilinear over the four neighbouring texels. Sampling a 1-bit source
      // this way is what turns an 8x8 icon into something that looks like it
      // is turning rather than flickering between four orientations.
      const int iu = floor_i(u), iv = floor_i(v);
      const float fu = u - static_cast<float>(iu);
      const float fv = v - static_cast<float>(iv);
      float a = 0.0f;
      a += (s.on(iu, iv) ? 1.0f : 0.0f) * (1.0f - fu) * (1.0f - fv);
      a += (s.on(iu + 1, iv) ? 1.0f : 0.0f) * fu * (1.0f - fv);
      a += (s.on(iu, iv + 1) ? 1.0f : 0.0f) * (1.0f - fu) * fv;
      a += (s.on(iu + 1, iv + 1) ? 1.0f : 0.0f) * fu * fv;
      if (a <= 0.002f) continue;
      const float cov = a * bright;
      if (b == Blend::Add) {
        fb.add_scaled(px, py, color, cov);
      } else {
        fb.blend(px, py, color, cov);
      }
    }
  }
}

// ------------------------------------------------------------- the spin

float spin_face_scale(float turns) {
  float c = fast_cos(turns);
  return c < 0.0f ? -c : c;
}

float spin_face_bright(float turns) {
  // A floor of 0.55 plus the Lambert term. The floor is not physical; it is
  // there because the panel has eight rows and a face that dims in proportion
  // to its area would be invisible for the middle third of the turn.
  return 0.55f + 0.45f * spin_face_scale(turns);
}

void draw_spin(Framebuffer& fb, const Sprite& from, const Sprite& to, float cx,
               float cy, float turns, RGB color, int clip_y0, int clip_y1) {
  const bool second_half = turns >= 0.25f;
  const Sprite& face = second_half ? to : from;
  if (!face.rows) return;

  const float sy = spin_face_scale(turns);
  const float bright = spin_face_bright(turns);

  // Sub-pixel heights below about a tenth of a row carry no ink worth drawing,
  // and the rim below covers the gap.
  if (sy > 0.02f) {
    draw_sprite_scaled(fb, face, cx, cy, 1.0f, sy, color, bright, clip_y0,
                       clip_y1, Ink::Normalised, Blend::Add);
  }

  // The rim: as the face passes edge-on it is a line one row high, and a real
  // disc catches the light along that line. Without it the glyph simply
  // disappears for a frame or two and the turn loses its middle.
  const float d = turns < 0.25f ? 0.25f - turns : turns - 0.25f;
  if (d < kSpinRimWidth) {
    const float k = 1.0f - d / kSpinRimWidth;
    const float rim = k * k;
    // Width of the rim follows the widest extent of either face, so it reads
    // as the edge of the same object rather than a stray line.
    float half = 0.5f * static_cast<float>(face.w);
    if (half < 0.5f) half = 0.5f;
    const uint8_t k8 = static_cast<uint8_t>(rim * 255.0f + 0.5f);
    fb.span_h(cx - half, cx + half, static_cast<int>(std::floor(cy)),
              color.scaled(k8), Blend::Add);
  }
}

// ------------------------------------------------------------- the pop

float pop_out_scale(float u) {
  if (u <= 0.0f) return 1.0f;
  if (u >= kPopSwapPoint) return 0.0f;
  // Accelerating collapse: it should look pulled in, not eased out.
  const float k = u / kPopSwapPoint;
  return 1.0f - k * k;
}

float pop_in_scale(float u) {
  if (u <= kPopSwapPoint) return 0.0f;
  if (u >= 1.0f) return 1.0f;
  const float k = (u - kPopSwapPoint) / (1.0f - kPopSwapPoint);
  return ease::out_back(k);
}

float SwapState::u(double now_s, float seconds) const {
  if (!primed) return 1.0f;
  const double d = now_s - t0;
  if (d <= 0.0) return 0.0f;
  if (d >= seconds) return 1.0f;
  return static_cast<float>(d / seconds);
}

void draw_icon_scaled(Framebuffer& fb, int x, int y, const Icon& ic, RGB color,
                      float sx, float sy, float bright) {
  if (sx <= 0.0f || sy <= 0.0f) return;
  const Sprite s = sprite_of(ic);
  draw_sprite_scaled(fb, s, static_cast<float>(x) + kIconW * 0.5f,
                     static_cast<float>(y) + kIconH * 0.5f, sx, sy, color, bright,
                     0, kHeight, Ink::Normalised, Blend::Add);
}

void mini_draw_text_squashed(Framebuffer& fb, int x0, int box_w, int y,
                             const char* str, RGB color, float sy, float bright,
                             Blend b) {
  if (!str || sy <= 0.0f || bright <= 0.0f) return;
  const int ink = mini_text_ink_width(str);
  int x = x0 + (box_w - ink) / 2;
  if (x < x0) x = x0;
  const float cy = static_cast<float>(y) + kMiniH * 0.5f;
  for (const char* p = str; *p; ++p) {
    const MiniGlyph* g = mini_glyph_for(*p);
    if (!g) {
      x += kMiniSpaceAdvance;
      continue;
    }
    const Sprite s = sprite_of(*g);
    draw_sprite_scaled(fb, s, static_cast<float>(x) + g->w * 0.5f, cy, 1.0f, sy,
                       color, bright, 0, kHeight, Ink::Normalised, b);
    x += g->w + kMiniGap;
  }
}

}  // namespace panel
