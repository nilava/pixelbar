#include "ui/settings_tree.h"

#include <cstdio>

#include "panel/config.h"

namespace ui {
namespace {

using panel::RGB;

// Toggles all render through the list path, so there is one way a value
// becomes text rather than a special case for the commonest kind.
const char* const kOffOn[] = {"OFF", "ON"};

// A note on the labels below, because it has caught three of them already:
// this font gives M and W five columns and most letters three, so a
// four-letter label containing either measures seventeen against a box
// fifteen wide and is clipped. WORK, AMPS, SHOW and MOVE all had to go. A
// test measures every one of them, so the next offender fails the build
// rather than the eye.

// Colours are per group rather than per setting. A setting inherits the
// colour of the thing it belongs to, so scrolling a group reads as one place
// rather than as a row of unrelated lights.
constexpr RGB kDisplay(255, 200, 60);
constexpr RGB kTimer(255, 138, 31);
constexpr RGB kScene(120, 200, 255);
constexpr RGB kMotion(140, 60, 255);
constexpr RGB kTouch(60, 220, 140);
constexpr RGB kClock(200, 200, 255);
constexpr RGB kNet(60, 200, 255);
constexpr RGB kLink(120, 140, 255);
constexpr RGB kSystem(255, 60, 40);
constexpr RGB kMedia(255, 90, 200);

#define ROW(icon, label, colour) {&panel::icon, label, colour, false}
#define SPIN(icon, label, colour) {&panel::icon, label, colour, true}

// Labels are four or five characters because that is what fits beside an icon
// in fifteen columns. mini_text_fits() is asserted over every one of them in
// the tests, so a label too wide to read fails the build rather than the eye.
const SettingDesc kDisplayItems[] = {
    {ROW(kIconSunCore, "DIM", kDisplay), SettingId::Brightness,
     SettingKind::Screen, panel::Screen::Brightness, 0, 0, 0, "", nullptr, 0},
    {ROW(kIconPalette, "HUE", kDisplay), SettingId::Hue, SettingKind::Screen,
     panel::Screen::ColorPick, 0, 0, 0, "", nullptr, 0},
    {ROW(kIconDisplay, "FLIP", kDisplay), SettingId::Flip, SettingKind::Toggle,
     panel::Screen::Count, 0, 1, 1, "", kOffOn, 2},
    // Zero means never, and it is the first value rather than the last so that
    // turning down from one minute lands on it.
    {ROW(kIconMoon, "REST", kDisplay), SettingId::SleepAfter,
     SettingKind::Number, panel::Screen::Count, 0, 120, 5, "M", nullptr, 0},
    // The current cap, in hundreds of milliamps: a 2.5 A design target reads
    // as 25. Four digits will not fit beside an icon and the exact figure is
    // not what anybody is choosing between.
    {ROW(kIconWarning, "CAP", kDisplay), SettingId::MaxMa, SettingKind::Number,
     panel::Screen::Count, static_cast<int16_t>(panel::kMinMilliamps / 100),
     static_cast<int16_t>(panel::kMaxAllowedMilliamps / 100), 1, "", nullptr,
     0},
};

const SettingDesc kTimerItems[] = {
    // Its own screen rather than a number in a rail: the timer length already
    // has a display that shows the figure the way the timer itself will, and
    // a generic adjuster would be a worse version of it.
    {ROW(kIconHourglass, "TASK", kTimer), SettingId::WorkMin,
     SettingKind::Screen, panel::Screen::TimerSet, 1, 99, 1, "M", nullptr, 0},
    {ROW(kIconMoon, "REST", kTimer), SettingId::RestMin, SettingKind::Number,
     panel::Screen::Count, 1, 60, 1, "M", nullptr, 0},
    {ROW(kIconGrid, "SETS", kTimer), SettingId::Cycles, SettingKind::Number,
     panel::Screen::Count, 1, 12, 1, "", nullptr, 0},
    {ROW(kIconFree, "AUTO", kTimer), SettingId::AutoNext, SettingKind::Toggle,
     panel::Screen::Count, 0, 1, 1, "", kOffOn, 2},
};

// The scene names come from the panel's own list rather than a second copy
// here, so adding a scene cannot leave the picker naming the wrong one.
const char* const kSceneNames[] = {"SOLID", "RAINBOW", "PLASMA", "SPARKLE"};

const SettingDesc kSceneItems[] = {
    {ROW(kIconGrid, "PICK", kScene), SettingId::Scene, SettingKind::List,
     panel::Screen::Count, 0, 3, 1, "", kSceneNames, 4},
};

const SettingDesc kMotionItems[] = {
    {ROW(kIconMotion, "USE", kMotion), SettingId::Motion, SettingKind::Toggle,
     panel::Screen::Count, 0, 1, 1, "", kOffOn, 2},
    {ROW(kIconMoon, "FLAT", kMotion), SettingId::FlatSleeps,
     SettingKind::Toggle, panel::Screen::Count, 0, 1, 1, "", kOffOn, 2},
};

const SettingDesc kTouchItems[] = {
    {ROW(kIconLock, "LOCK", kTouch), SettingId::TouchLock, SettingKind::Toggle,
     panel::Screen::Count, 0, 1, 1, "", kOffOn, 2},
};

const char* const kHourNames[] = {"12H", "24H"};
const SettingDesc kClockItems[] = {
    {ROW(kIconInfo, "HRS", kClock), SettingId::Clock24h, SettingKind::List,
     panel::Screen::Count, 0, 1, 1, "", kHourNames, 2},
};

// Actions carry no range, no unit and no options: the last seven fields are
// inert for them, and setting_get/setting_set ignore the ids entirely. The
// round-trip test skips them for that reason, the way it already skips the
// ones that open a screen.
#define ACTION(icon, label, colour, id_)                                   \
  {ROW(icon, label, colour), SettingId::id_, SettingKind::Action,           \
   panel::Screen::Count, 0, 0, 0, "", nullptr, 0}

// Four or five characters have to carry a whole verb, so they are chosen to
// read as one vocabulary rather than as abbreviations of unrelated words:
// DROP forgets one thing, WIPE forgets everything, and the group says what the
// thing is. "FGET" and "CLR" and "SEEN" were none of that — they were what was
// left after squeezing, and needed explaining, which a label that needs
// explaining has already failed at.
const SettingDesc kNetItems[] = {
    // First, because it is the one that decides whether the rest of the group
    // means anything.
    {ROW(kIconDownload, "USE", kNet), SettingId::WifiOn, SettingKind::PortToggle,
     panel::Screen::Count, 0, 1, 1, "", kOffOn, 2},
    {ROW(kIconInfo, "IP", kNet), SettingId::Count, SettingKind::Screen,
     panel::Screen::WifiInfo, 0, 0, 0, "", nullptr, 0},
    // Forgetting the network is how a panel moves house, and the only way back
    // in afterwards is the setup AP or Bluetooth — hence the confirm.
    ACTION(kIconCross, "DROP", kNet, ActionForgetWifi),
};

const SettingDesc kLinkItems[] = {
    ACTION(kIconDownload, "ADD", kLink, ActionPair),
    {ROW(kIconGrid, "LIST", kLink), SettingId::Count, SettingKind::Screen,
     panel::Screen::Paired, 0, 0, 0, "", nullptr, 0},
    ACTION(kIconCross, "DROP", kLink, ActionForgetHosts),
};

// Its own group, and last.
//
// Not tucked in beside "forget the network", which is the neighbour it most
// resembles and the reason to keep it apart: one of them costs you a Wi-Fi
// password and the other costs you every pairing on every host. A group of its
// own means reaching it is a decision rather than an overshoot.
const SettingDesc kSystemItems[] = {
    ACTION(kIconWarning, "WIPE", kSystem, ActionFactory),
};

#define GROUP(row_, items_) \
  {row_, items_, static_cast<uint8_t>(sizeof(items_) / sizeof(items_[0])), panel::Screen::Count}
#define SHORTCUT(row_, screen_) {row_, nullptr, 0, screen_}

}  // namespace

const SettingGroup kGroups[] = {
    SHORTCUT(ROW(kIconBusy, "STAT", RGB(255, 30, 15)), panel::Screen::StatusPick),
    GROUP(ROW(kIconSunCore, "DISP", kDisplay), kDisplayItems),
    GROUP(ROW(kIconHourglass, "TIME", kTimer), kTimerItems),
    GROUP(ROW(kIconGrid, "IDLE", kScene), kSceneItems),
    GROUP(ROW(kIconMotion, "TILT", kMotion), kMotionItems),
    GROUP(ROW(kIconHand, "TAP", kTouch), kTouchItems),
    GROUP(SPIN(kIconGear, "CLCK", kClock), kClockItems),
    GROUP(ROW(kIconDownload, "WIFI", kNet), kNetItems),
    // HOST rather than LINK: what this group manages is the machines allowed
    // to drive the panel, and "link" named the transport instead of the thing.
    GROUP(ROW(kIconLock, "HOST", kLink), kLinkItems),
    // A shortcut rather than a group: there is nothing to configure, only a
    // screen to be on. Same shape as the status picker, and the same reason.
    SHORTCUT(ROW(kIconSunRays, "PLAY", kMedia), panel::Screen::Media),
    GROUP(ROW(kIconWarning, "SYS", kSystem), kSystemItems),
};
const int kGroupCount = static_cast<int>(sizeof(kGroups) / sizeof(kGroups[0]));

int setting_get(const Settings& s, SettingId id) {
  switch (id) {
    case SettingId::Brightness: return s.brightness;
    // Scaled to whole percent. The adjuster works in integers so that every
    // setting shares one piece of arithmetic, and a hue is the only float
    // here — a hundred steps around the wheel is finer than the panel can
    // show anyway.
    case SettingId::Hue: return static_cast<int>(s.hue * 100.0f + 0.5f);
    case SettingId::Flip: return s.flip ? 1 : 0;
    case SettingId::SleepAfter: return s.sleep_after_min;
    case SettingId::MaxMa: return s.max_ma / 100;
    case SettingId::WorkMin: return s.work_min;
    case SettingId::RestMin: return s.rest_min;
    case SettingId::Cycles: return s.cycles;
    case SettingId::AutoNext: return s.auto_next ? 1 : 0;
    case SettingId::Scene: return s.scene;
    case SettingId::Motion: return s.motion_enabled ? 1 : 0;
    case SettingId::FlatSleeps: return s.flat_sleeps ? 1 : 0;
    case SettingId::TouchLock: return s.touch_locked ? 1 : 0;
    case SettingId::Clock24h: return s.clock_24h ? 1 : 0;
    // Actions hold nothing. Reading one is not an error — the row renderer
    // asks every descriptor for a value — it just has nothing to say.
    case SettingId::ActionPair:
    case SettingId::ActionForgetHosts:
    case SettingId::ActionForgetWifi:
    case SettingId::ActionFactory:
    // Behind the port, not in the struct. See SettingKind::PortToggle.
    case SettingId::WifiOn:
    case SettingId::Count: break;
  }
  return 0;
}

void setting_set(Settings& s, SettingId id, int v) {
  switch (id) {
    case SettingId::Brightness: s.brightness = static_cast<uint8_t>(v); break;
    case SettingId::Hue: s.hue = static_cast<float>(v) / 100.0f; break;
    case SettingId::Flip: s.flip = v != 0; break;
    case SettingId::SleepAfter: s.sleep_after_min = static_cast<uint16_t>(v); break;
    case SettingId::MaxMa: s.max_ma = static_cast<uint16_t>(v * 100); break;
    case SettingId::WorkMin: s.work_min = static_cast<uint16_t>(v); break;
    case SettingId::RestMin: s.rest_min = static_cast<uint16_t>(v); break;
    case SettingId::Cycles: s.cycles = static_cast<uint8_t>(v); break;
    case SettingId::AutoNext: s.auto_next = v != 0; break;
    case SettingId::Scene: s.scene = static_cast<uint8_t>(v); break;
    case SettingId::Motion: s.motion_enabled = v != 0; break;
    case SettingId::FlatSleeps: s.flat_sleeps = v != 0; break;
    case SettingId::TouchLock: s.touch_locked = v != 0; break;
    case SettingId::Clock24h: s.clock_24h = v != 0; break;
    case SettingId::ActionPair:
    case SettingId::ActionForgetHosts:
    case SettingId::ActionForgetWifi:
    case SettingId::ActionFactory:
    // Behind the port, not in the struct. See SettingKind::PortToggle.
    case SettingId::WifiOn:
    case SettingId::Count: break;
  }
  // Every write goes through the same clamp the loader uses, so a value that
  // could not survive a power cut cannot exist in memory either.
  s.sanitise();
}

const char* setting_text(const Settings& s, const SettingDesc& d, char* out,
                         int cap) {
  const int v = setting_get(s, d.id);
  if (d.options && v >= 0 && v < d.option_count) {
    snprintf(out, cap, "%s", d.options[v]);
    return out;
  }
  // A screen or an action has no value to print; the row is the whole thing.
  if (d.kind == SettingKind::Action) {
    snprintf(out, cap, "%s", "");
    return out;
  }
  if (d.kind == SettingKind::Screen) {
    snprintf(out, cap, "%d", v);
    return out;
  }
  // Zero on a duration means "never" rather than "immediately", and saying so
  // is the difference between a setting that reads as off and one that reads
  // as broken.
  if (v == 0 && d.unit && d.unit[0] == 'M') {
    snprintf(out, cap, "OFF");
    return out;
  }
  snprintf(out, cap, "%d%s", v, d.unit ? d.unit : "");
  return out;
}

float setting_fraction(const Settings& s, const SettingDesc& d) {
  if (d.kind != SettingKind::Number) return -1.0f;
  if (d.hi <= d.lo) return -1.0f;
  const float f = static_cast<float>(setting_get(s, d.id) - d.lo) /
                  static_cast<float>(d.hi - d.lo);
  return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
}

}  // namespace ui
