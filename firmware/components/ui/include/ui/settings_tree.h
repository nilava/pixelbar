// The settings, as data rather than as code.
//
// There are fourteen of them and there will be more. Written out as a screen
// each, that is fourteen draw functions differing by a label and a range —
// and fourteen places for the next one to be forgotten. So a setting is a
// descriptor: what it is called, what it looks like, what kind of value it
// holds, and an id the one get/set pair switches on. Adding a setting is a
// row in a table and a case in two switches, and the compiler names the
// switches if you forget.
//
// This lives in `ui` rather than `panel` because it is about the Settings
// struct, which the drawing layer must not know exists. What crosses to the
// panel is the already-rendered result: a label, a colour, and the text the
// value reads as.
#pragma once
#include <cstdint>

#include "panel/screens.h"
#include "ui/settings.h"

namespace ui {

enum class SettingId : uint8_t {
  // Display
  Brightness,
  Hue,
  Flip,
  SleepAfter,
  MaxMa,
  // Timer
  WorkMin,
  RestMin,
  Cycles,
  AutoNext,
  // Scene
  Scene,
  // Motion
  Motion,
  FlatSleeps,
  // Touch
  TouchLock,
  // Clock
  Clock24h,
  Count,
};

enum class SettingKind : uint8_t {
  // Hands off to a screen of its own, because the value deserves better than a
  // number: brightness and hue both have pickers that show you the thing
  // itself rather than a figure describing it.
  Screen,
  Toggle,
  Number,
  List,
};

struct SettingDesc {
  panel::MenuEntry row;  // icon, label, colour, whether it turns
  SettingId id;
  SettingKind kind;

  panel::Screen screen;  // Kind::Screen

  // Kind::Number. `unit` is a single trailing character — M for minutes, % for
  // a percentage — or an empty string. There is no room for a word.
  int16_t lo, hi, step;
  const char* unit;

  // Kind::List. Also used by Toggle, which is a list of two whose options are
  // always the same, so that the value rendering has one path and not two.
  const char* const* options;
  uint8_t option_count;
};

struct SettingGroup {
  panel::MenuEntry row;
  const SettingDesc* items;
  uint8_t count;
  // A group that is really a shortcut. Status is not a setting — it is not
  // stored and it changes twenty times a day — but it belongs at the top of
  // the menu, and burying it one level down to keep the tree uniform would
  // cost the most-used path two presses to save an if. Screen::Count means an
  // ordinary group with a list under it.
  panel::Screen direct;
};

extern const SettingGroup kGroups[];
extern const int kGroupCount;

// The value as an integer, whatever the field's real type. A float hue is
// scaled to 0..100 so the adjuster has one kind of arithmetic; the descriptor
// is what says how to read it back.
int setting_get(const Settings& s, SettingId id);
// Clamped by the descriptor, so no caller can store a value the screen could
// not have produced.
void setting_set(Settings& s, SettingId id, int value);

// What the value reads as on the panel: "ON", "25M", "PLASMA". Writes into
// `out` and returns it, so the caller owns the storage and nothing here needs
// a static buffer that two screens could fight over.
const char* setting_text(const Settings& s, const SettingDesc& d, char* out,
                         int cap);

// Where the value sits in its range, 0..1, or negative when it has no range —
// which is what stops a toggle drawing a rail it has no position along.
float setting_fraction(const Settings& s, const SettingDesc& d);

}  // namespace ui
