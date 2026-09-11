#include "ui/virtual_pads.h"

namespace ui {
namespace {

// The drag script, in seconds from its start. A finger crossing the pads holds
// two at once briefly and lets the first go before reaching the third, which is
// exactly what tells a swipe from a chord.
struct DragStep {
  float at;
  int8_t pad;   // index into the direction-ordered sequence
  bool down;
};
const DragStep kDrag[] = {
    {0.00f, 0, true},  {0.06f, 1, true},  {0.09f, 0, false},
    {0.12f, 2, true},  {0.15f, 1, false}, {0.21f, 2, false},
};
constexpr int kDragSteps = static_cast<int>(sizeof(kDrag) / sizeof(kDrag[0]));
constexpr float kDragEnd = 0.24f;

}  // namespace

void VirtualPads::tap(int zone) {
  if (zone < 0 || zone >= kZones) return;
  tap_left_[zone] = kVirtualTapSeconds;
}

void VirtualPads::set_held(int zone, bool held) {
  if (zone < 0 || zone >= kZones) return;
  held_[zone] = held;
}

void VirtualPads::release_all() {
  for (int i = 0; i < kZones; ++i) {
    held_[i] = false;
    tap_left_[i] = 0.0f;
  }
  drag_step_ = -1;
}

void VirtualPads::swipe(int dir) {
  drag_dir_ = dir >= 0 ? 1 : -1;
  drag_step_ = 0;
  drag_t_ = 0.0f;
  for (int i = 0; i < kZones; ++i) tap_left_[i] = 0.0f;
}

void VirtualPads::advance(float dt_s) {
  if (dt_s < 0.0f) dt_s = 0.0f;
  for (int i = 0; i < kZones; ++i) {
    if (tap_left_[i] > 0.0f) {
      tap_left_[i] -= dt_s;
      if (tap_left_[i] < 0.0f) tap_left_[i] = 0.0f;
    }
  }

  if (drag_step_ < 0) return;
  drag_t_ += dt_s;
  while (drag_step_ < kDragSteps && drag_t_ >= kDrag[drag_step_].at) {
    const DragStep& st = kDrag[drag_step_];
    const int pad = drag_dir_ > 0 ? st.pad : (kZones - 1 - st.pad);
    held_[pad] = st.down;
    ++drag_step_;
  }
  if (drag_t_ >= kDragEnd) {
    drag_step_ = -1;
    for (int i = 0; i < kZones; ++i) held_[i] = false;
  }
}

void VirtualPads::apply(bool* touch, int count) const {
  if (!touch) return;
  const int n = count < kZones ? count : kZones;
  for (int i = 0; i < n; ++i) {
    if (held_[i] || tap_left_[i] > 0.0f) touch[i] = true;
  }
}

bool VirtualPads::any_down() const {
  for (int i = 0; i < kZones; ++i)
    if (held_[i] || tap_left_[i] > 0.0f) return true;
  return false;
}

}  // namespace ui
