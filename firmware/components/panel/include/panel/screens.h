// The screens the UI shows, as opposed to the ambient patterns.
//
// Every screen is a pure function of UiState and the clock, so the input layer
// and the state machine can be developed and tested separately from drawing.
#pragma once
#include <cstdint>

#include "panel/anim.h"
#include "panel/color.h"
#include "panel/digit_roll.h"
#include "panel/framebuffer.h"
#include "panel/patterns.h"
#include "panel/icons.h"
#include "panel/sprite.h"

namespace panel {

enum class Status : uint8_t {
  // The four that fit beside an 8 px icon: their labels are 15 px or less.
  Free, Busy, Call, Dnd,
  // And the ones that do not. These take the whole panel as a knocked-out
  // badge instead; see status_uses_badge.
  Away, Focus, Lunch, Meet,
  Count,
};

const char* status_label(Status s);
RGB status_color(Status s);

// The colour the picker's cursor is sitting on. Without this the picker moves
// a cursor over a hue it has no way to apply.
RGB accent_from_hue(float hue);

// One row of a scrolled list: an icon and a word.
//
// The list itself lives in `ui`, not here. There used to be a kMenu[] in this
// file as well, and once the settings tree landed it was a four-entry menu the
// device no longer had — still rendered whenever nothing else was supplied.
// The drawing layer now draws the list it is given and nothing when given
// none, which is the only arrangement with one source of truth in it.
struct MenuEntry {
  const Icon* icon;
  const char* label;
  RGB color;
  bool spins;  // drawn through draw_sprite_rotated, so it turns while you look
};

// One row of a scrolled list: icon, label, and ticks showing the position.
// Shared by Menu and Group, which differ only in what they are a list of —
// duplicating it would mean the two levels of the same list drifting apart.
void draw_list_row(Framebuffer& fb, const MenuEntry& e, int idx, int count,
                   const Anim& a);

// Text in a box, scrolled only when it does not fit.
//
// Was open-coded identically in two places with the rate written out twice; a
// third screen needing it made that a pattern rather than a coincidence.
//
// One pass with a gap rather than a continuous belt: a name you are trying to
// read off a shelf needs to start somewhere, and a loop with no beginning is
// hard to catch. Text that fits is centred and still, because scrolling
// something legible is just movement.
void draw_marquee(Framebuffer& fb, const Anim& a, const char* text, int x0,
                  int box_w, int y, RGB color);

// The ambient scenes, in the order the picker offers them. A separate list
// from Pattern because not every pattern is a scene: Text and Clock duplicate
// screens that already exist, and MapTest is a diagnostic rather than
// something anybody would choose to look at.
// An icon by name, for hosts that can send a word but not a bitmap. Returns
// nullptr for anything unknown, which the caller should treat as "no icon"
// rather than as an error: a host naming an icon this firmware does not have is
// a version skew, not a fault.
const Icon* icon_by_name(const char* name);

Pattern scene_pattern(int index);
const char* scene_name(int index);
int scene_count();

// True when the label is too wide to sit beside an icon, so the status is
// drawn as a full-width badge instead. Derived from the label's measured
// width rather than listed, so adding a status cannot get this wrong.
bool status_uses_badge(Status s);

enum class Screen : uint8_t {
  Status,      // the room-facing default: one word, one colour
  Clock,
  Timer,       // focus countdown
  Menu,        // the settings groups: the icon-and-label list, scrolled like a record
  Group,       // the settings inside one group, drawn as the same list
  Setting,     // one setting being changed: toggle, number or choice
  Scene,       // an ambient pattern, full panel
  StatusPick,  // choosing a status: the same list, drawn as the status itself
  Brightness,  // encoder adjust
  ColorPick,   // encoder adjust
  TimerSet,    // encoder adjust
  Sleep,
  Booting,
  // The network, when it needs something from you. Not in the home carousel:
  // these are states the device is in, not views you choose between.
  WifiSetup,       // no credentials: join this network and open the page
  WifiConnecting,  // trying the credentials someone just typed
  WifiInfo,
  WifiFailed,      // that network said no, and why        // the address, so you can reach the page from your own network
  OtaProgress,     // a firmware update is being written; do not unplug it
  Pairing,         // the six digits a Bluetooth host must be told
  Draw,            // whatever a host asked the panel to show
  Paired,          // the hosts this panel has been paired with
  Confirm,         // are you sure — a knob has no undo
  Count,
};

const char* screen_name(Screen s);

struct UiState {
  Status status = Status::Free;
  RGB accent{255, 138, 31};

  // Focus timer.
  int timer_total_s = 25 * 60;
  int timer_left_s = 25 * 60;
  bool timer_running = false;
  // Which half of the cycle, and which round of the set.
  //
  // The settings for these — rest minutes, how many rounds, whether to carry
  // on by itself — have been stored, sanitised and settable from the menu
  // since the settings tree landed, and until now nothing read them: the
  // timer counted work down once and stopped. A pomodoro that does not rest
  // is a countdown.
  bool timer_resting = false;
  uint8_t timer_cycle = 1;   // 1-based, counts work rounds
  uint8_t timer_cycles = 4;  // mirrored from Settings so the screen can draw it

  // Adjustable settings.
  uint8_t brightness = 48;
  float hue = 0.08f;      // cursor position on the colour picker, 0..1
  int timer_set_min = 25;

  // Which menu entry is under the cursor. The list scrolls by changing this
  // and restarting the screen with a disk transition, so the outgoing frame
  // still holds the previous entry — which is the whole reason go_to and
  // restart_with take the state the panel is leaving.
  uint8_t menu_index = 0;
  // Which status the picker is sitting on, which is not the same as the status
  // the panel is showing: you scroll past several before pressing one.
  Status pick = Status::Free;

  // Seconds since power-on, for the boot sequence. Its own clock rather than
  // the animation clock, because it has to run once and finish.
  float boot_t = 0.0f;

  // Wall clock and link state, filled in by the caller.
  int hour = 0;
  int minute = 0;
  int second = 0;
  // Whether those three mean anything yet. Before the first SNTP sync there is
  // no time at all, and the Clock screen draws dashes rather than 00:00 — a
  // clock that is confidently wrong is worse than one that admits it is
  // waiting.
  bool time_valid = false;

  // The text the network screens marquee: the setup SSID to join, or the
  // address to visit. One field because only one of them is ever on screen,
  // and the screen itself says which it is.
  const char* net_text = "";

  // The list the Menu and Group screens draw, and where you are in it. Held as
  // a pointer so the two levels share one screen's worth of drawing code and
  // the model decides what is in the list.
  const MenuEntry* list = nullptr;
  int list_count = 0;

  // The setting being changed, flattened for the draw layer.
  //
  // Deliberately pre-rendered: the model works out what the value *reads* as —
  // "ON", "25M", "PLASMA" — and this layer only draws it. That is what lets
  // one Setting screen serve toggles, numbers and choices rather than three
  // screens differing by a few pixels, and it keeps the knowledge of what a
  // setting means on the side of the seam that has the Settings struct.
  const Icon* set_icon = nullptr;
  const char* set_label = "";
  const char* set_text = "";
  RGB set_tint{255, 138, 31};
  // 0..1 fills the rail along the bottom; negative draws no rail, which is
  // what a toggle or a short list wants.
  float set_fraction = -1.0f;
  bool set_on = false;

  // Which ambient pattern the Scene screen draws.
  uint8_t scene = 0;

  // The paired hosts, and which one is under the cursor. Names come from the
  // network component, so they are borrowed rather than owned — they outlive
  // the frame and change only when something pairs or is forgotten.
  const char* const* paired = nullptr;
  int paired_count = 0;
  int paired_index = 0;

  // The confirm screen: what is about to happen, and which way the cursor is
  // pointing. Defaulting to no is the whole point.
  // Why a join was refused. Borrowed, like net_text.
  const char* net_error = "";

  const char* confirm_label = "";
  bool confirm_yes = false;

  // How far through a firmware update, 0..1, or negative when none is running.
  float ota = -1.0f;

  // The Bluetooth pairing code, or 0 when nothing is pairing. Six digits, and
  // the only place they exist: that is what makes the bond mean the host is in
  // the room rather than merely in range.
  uint32_t passkey = 0;

  // What a host asked the panel to show, flattened for the draw layer the same
  // way a setting is: the model decides what the words are and this layer only
  // draws them. `draw_text` is owned by the model, not borrowed — unlike
  // net_text, which points at a static in the network component.
  const char* draw_text = "";
  const Icon* draw_icon = nullptr;
  RGB draw_tint{255, 138, 31};
  // Seconds remaining on a countdown, or negative for none. The model works it
  // out from a deadline and the wall clock; the panel just renders mm:ss.
  int draw_seconds = -1;
  // 0..1 draws a rail, negative draws none.
  float draw_bar = -1.0f;
  bool wifi_connected = false;
};

// The boot sequence: a spark at the hub, the disk winding up, then it settles.
// Its shape is the same radial geometry the view cycle turns on — see the
// Booting arm in screens.cpp.
constexpr float kBootSparkS = 0.34f;
constexpr float kBootSeconds = 1.9f;
constexpr int kBootSpokes = 3;
// Enough turns to read as spinning up rather than as one sweep.
constexpr float kBootRevolutions = 3.2f;
// A spoke is drawn at this many recent angles so it smears into a streak. At 24
// columns a fast single-pixel line reads as flicker rather than as movement.
constexpr int kBootTrail = 7;
constexpr float kBootTrailStep = 0.006f;

// The icon occupies columns 0..7, column 8 is a gutter that is never written,
// and the label box is columns 9..23.
constexpr int kIconX = 0;
constexpr int kGutterX = 8;
constexpr int kLabelX = 9;

// Per-screen animation state: digit rolls and the values that glide toward
// their targets. A screen cannot be a pure function of time and also ease
// toward a value that changed at an arbitrary moment, so this is its memory.
struct ScreenAnim {
  PairFaceAnim face;
  Smoothed bar{0.0f, 0.12f};
  Smoothed cursor{0.0f, 0.08f};
  Smoothed rays{0.0f, 0.12f};
  SmoothedRGB tint;
  bool primed = false;

  // The status the screen is currently showing, and the one it is turning over
  // from. These are the screen's own memory of the change: UiState only ever
  // holds the truth as of now, so without them a swap has nothing to swap from.
  SwapState swap;
  Status shown = Status::Free;
  Status was = Status::Free;
};

// The animated draw. Everything on every screen is in continuous motion.
void draw_screen(Framebuffer& fb, Screen s, const UiState& ui, const Anim& a,
                 ScreenAnim& sa);

// Still form, kept so existing callers and tests compile. Uses a scratch
// ScreenAnim, so anything eased is drawn already settled.
void draw_screen(Framebuffer& fb, Screen s, const UiState& ui, uint32_t now_ms);

// Layout helpers, shared with the pattern set and exercised by the tests.

// Two 2-digit groups separated by a colon, centred. Each group is clamped to
// 0..99, never wrapped, so a 25 minute timer reads 25 and not 01.
void draw_pair_face(Framebuffer& fb, int left, int right, bool show_colon, RGB color);

// hh:mm in 3x5 digits, centred, with the colon shown when show_colon is set.
void draw_clock_face(Framebuffer& fb, int hour, int minute, bool show_colon, RGB color);

// A filled rounded rectangle: the corners are simply omitted, which at eight
// rows is all a radius can mean.
void draw_badge(Framebuffer& fb, int x0, int y0, int w, int h, RGB fill);

// The full-width badge with sub-pixel top and bottom edges, so it can collapse
// to nothing smoothly. Rounding the height to whole rows would step through
// seven states in a fifth of a second, which is exactly the stepping the rest
// of this pipeline exists to avoid.
void draw_badge_aa(Framebuffer& fb, float top, float bottom, RGB fill);

// Where a full-width badge sits: rows 0..6, so its centre lands at 3.5 —
// the same centre a 5-row label at y = 1 has. Row 7 is left for a rail.
constexpr int kBadgeTop = 0;
constexpr int kBadgeRows = 7;
constexpr int kBadgeTextY = 1;

// The badge fill is held below full value on purpose.
//
// A full-width badge lights about 150 LEDs where the icon layout lights 48, and
// at full value the brighter statuses draw over 3.2 A — past the 2500 mA cap.
// The cap would handle it, but it would scale *only* the badge statuses, so
// switching between BUSY and FOCUS would visibly change how bright the whole
// panel is. Pulling the fill down here keeps every status under the cap and
// therefore at a consistent brightness. It costs nothing to read: on a field
// this large it is the area doing the work, not the intensity.
constexpr float kBadgeFill = 0.85f;

// The same, with a word cut out of it — the fill is drawn, then the glyphs are
// written in black over the top.
//
// Colour carries the meaning here, not a picture, which is what lets a word too
// wide for the 15 px label box fit anyway: the whole 24 columns are available
// because nothing else is competing for them. It also reads from much further
// away than ink on black does, because the lit area is twenty times larger.
void draw_badge_label(Framebuffer& fb, int y0, int h, const char* s, RGB fill);

// A horizontal bar across rows y0..y1 inclusive, filled left to right.
void draw_bar(Framebuffer& fb, int y0, int y1, float fraction, RGB on, RGB off);

// The same bar with a sub-pixel right edge: at fraction 0.517 the boundary LED
// is lit about 40% of the way from off to on, so the bar moves continuously
// instead of snapping between 24 positions.
void draw_bar_aa(Framebuffer& fb, int y0, int y1, float fraction, RGB on, RGB off);

// Right-aligned fixed-width number in 3x5 digits. Leading zeros are dropped.
void draw_tiny_number(Framebuffer& fb, int x, int y, int value, int digits, RGB color);
int tiny_number_width(int digits);

}  // namespace panel
