#include "board/board.h"

#include <cstdio>
#include <cmath>
#include <cstring>
#include <ctime>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "hal/gpio_ll.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "net/net.h"
#include "pins.h"

namespace board {
namespace {

const char* TAG = "hal";

// ------------------------------------------------------------- encoder
//
// Decoded in an interrupt rather than polled, because the ESP32-C3 has no PCNT
// peripheral — SOC_PCNT_SUPPORTED is defined for the S3 and absent here. At the
// 10 ms frame tick a brisk flick of 20 detents a second puts an edge every
// 12.5 ms, so polling would drop detents exactly when the knob is being spun
// hardest. The interrupt accumulates and the frame drains it, so a late frame
// loses nothing.

ui::Quadrature g_quad;
volatile int32_t g_detents = 0;
volatile uint32_t g_illegal = 0;

// In IRAM, along with everything it touches — and that second clause has to be
// true rather than merely intended.
//
// An interrupt living in flash stalls for the milliseconds the cache is
// disabled during an NVS commit, and on an encoder that is detents silently
// going missing. Worse than missing detents, though, is what happens during a
// firmware update: the cache is off for far longer, and a flash access from an
// interrupt there is not a stall but a Cache error panic. That is exactly how
// the first over-the-air update died — the ISR was correctly marked IRAM_ATTR
// and correctly registered with ESP_INTR_FLAG_IRAM, and then called
// gpio_get_level(), which lives in flash. The comment above this function
// already claimed otherwise and was wrong.
//
// gpio_ll_get_level is a static inline that compiles down to one register
// read, so it lands inside this function rather than being a call out to
// somewhere the cache may not be able to reach. Quadrature::update is
// header-inline for the same reason.
void IRAM_ATTR encoder_isr(void* /*arg*/) {
  const bool a = gpio_ll_get_level(&GPIO, pins::kEncoderA) != 0;
  const bool b = gpio_ll_get_level(&GPIO, pins::kEncoderB) != 0;
  g_quad.update(a, b);
  g_detents = g_quad.detents();
  g_illegal = g_quad.illegal();
}

esp_err_t init_encoder() {
  gpio_config_t in = {};
  in.pin_bit_mask = (1ULL << pins::kEncoderA) | (1ULL << pins::kEncoderB);
  in.mode = GPIO_MODE_INPUT;
  in.pull_up_en = GPIO_PULLUP_ENABLE;  // EC11 contacts pull to ground
  in.intr_type = GPIO_INTR_ANYEDGE;
  ESP_RETURN_ON_ERROR(gpio_config(&in), TAG, "encoder pins");

  gpio_config_t sw = {};
  sw.pin_bit_mask = 1ULL << pins::kEncoderSw;
  sw.mode = GPIO_MODE_INPUT;
  sw.pull_up_en = GPIO_PULLUP_ENABLE;
  sw.intr_type = GPIO_INTR_DISABLE;  // the switch is read at frame rate
  ESP_RETURN_ON_ERROR(gpio_config(&sw), TAG, "encoder switch");

  // Prime the decoder on the resting position, so the first movement is
  // measured from where the knob actually is rather than from zero.
  g_quad.update(gpio_get_level(pins::kEncoderA) != 0,
                gpio_get_level(pins::kEncoderB) != 0);

  esp_err_t err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
  if (err == ESP_ERR_INVALID_STATE) err = ESP_OK;  // already installed
  ESP_RETURN_ON_ERROR(err, TAG, "isr service");
  ESP_RETURN_ON_ERROR(gpio_isr_handler_add(pins::kEncoderA, encoder_isr, nullptr),
                      TAG, "isr A");
  ESP_RETURN_ON_ERROR(gpio_isr_handler_add(pins::kEncoderB, encoder_isr, nullptr),
                      TAG, "isr B");
  return ESP_OK;
}

// ------------------------------------------------------------- touch pads
//
// Configured as inputs whether or not the modules are fitted. A TTP223 drives
// its output actively, so a pull-down is what keeps an unpopulated pad reading
// low instead of floating and inventing touches.
esp_err_t init_touch() {
  gpio_config_t t = {};
  t.pin_bit_mask = (1ULL << pins::kTouchLeft) | (1ULL << pins::kTouchMiddle) |
                   (1ULL << pins::kTouchRight);
  t.mode = GPIO_MODE_INPUT;
  t.pull_down_en = GPIO_PULLDOWN_ENABLE;
  t.intr_type = GPIO_INTR_DISABLE;
  return gpio_config(&t);
}

// -------------------------------------------------------------- motion
//
// An MPU-6050 on I²C, read once a frame for its accelerometer only. The gyro
// is powered but unused: nothing above this asks for rotation rate, and the
// three gestures that exist — a knock on the case, a shake, and being laid
// flat — all fall out of the acceleration vector alone.
//
// Range is ±4 g rather than the ±2 g default, and that is a consequence of the
// thresholds rather than a preference. tap_g and shake_g are *jolts* — changes
// in the magnitude of the vector, 1.2 g and 1.8 g — and the panel is sitting at
// 1 g before the knock arrives, so a shake worth recognising takes the
// instantaneous magnitude close to 3 g. At ±2 g that clips, and a clipped peak
// reads as a smaller jolt than it was: the harder you hit it, the less it would
// notice.
constexpr uint8_t kMpuAddr = 0x68;
// Which part actually answered.
//
// The board fitted here reports 0x70, which is an MPU-6500 — a great many
// modules sold as "GY-521 / MPU-6050" carry 6500 silicon, and the two are
// register-compatible for everything used below. Worth naming rather than
// waving through: they differ in exactly one place that matters, and a driver
// that assumed 6050 would configure the accelerometer filter on a register the
// 6500 does not use for it, leaving the panel listening to the desk.
constexpr uint8_t kWho6050 = 0x68;
constexpr uint8_t kWho6500 = 0x70;
constexpr uint8_t kWho9250 = 0x71;
constexpr uint8_t kRegSmplrtDiv = 0x19;
constexpr uint8_t kRegConfig = 0x1A;
constexpr uint8_t kRegAccelConfig = 0x1C;
// MPU-6500 and 9250 only. On those parts CONFIG's DLPF filters the gyro and
// temperature alone and the accelerometer has its own; on the 6050 one filter
// serves both and 0x1D is not this register, so it is written only when the
// part that has it says so.
constexpr uint8_t kRegAccelConfig2 = 0x1D;
constexpr uint8_t kRegAccelXoutH = 0x3B;
constexpr uint8_t kRegPwrMgmt1 = 0x6B;
constexpr uint8_t kRegWhoAmI = 0x75;
// ±4 g, so 32768 counts over 4 g.
constexpr float kAccelLsbPerG = 8192.0f;

i2c_master_bus_handle_t g_i2c_bus = nullptr;
i2c_master_dev_handle_t g_mpu = nullptr;
// Whether the sensor answered. Distinct from pins::kMotionFitted, which is
// what the build was *told*: a device that has been declared and does not
// reply must be absent rather than wrong, for the same reason an unfitted pin
// is never read. It also stops a missing sensor costing a bus timeout every
// frame, which would show up as the frame rate collapsing rather than as
// anything to do with motion.
bool g_mpu_ok = false;

esp_err_t mpu_write(uint8_t reg, uint8_t val) {
  const uint8_t buf[2] = {reg, val};
  return i2c_master_transmit(g_mpu, buf, sizeof(buf), 100);
}

esp_err_t mpu_read(uint8_t reg, uint8_t* out, size_t n) {
  return i2c_master_transmit_receive(g_mpu, &reg, 1, out, n, 100);
}

esp_err_t init_motion() {
  if (!pins::kMotionFitted) return ESP_OK;

  i2c_master_bus_config_t bus = {};
  bus.i2c_port = I2C_NUM_0;
  bus.sda_io_num = pins::kI2cSda;
  bus.scl_io_num = pins::kI2cScl;
  bus.clk_source = I2C_CLK_SRC_DEFAULT;
  bus.glitch_ignore_cnt = 7;
  // The GY-521 carries its own pull-ups. Enabling the internal ones as well is
  // harmless and is what keeps the bus defined if a bare chip is used instead.
  bus.flags.enable_internal_pullup = true;
  ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus, &g_i2c_bus), TAG, "i2c bus");

  i2c_device_config_t dev = {};
  dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  dev.device_address = kMpuAddr;
  dev.scl_speed_hz = 400000;
  ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(g_i2c_bus, &dev, &g_mpu), TAG,
                      "i2c device");

  // Who is there, before trusting anything it says. A wrong or absent answer
  // is reported and then left alone; the panel works without motion and the
  // settings tree already has a switch for turning it off.
  uint8_t who = 0;
  esp_err_t err = mpu_read(kRegWhoAmI, &who, 1);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "no MPU-6050 on I2C (SDA %d, SCL %d): %s — motion is off",
             pins::kI2cSda, pins::kI2cScl, esp_err_to_name(err));
    return ESP_OK;
  }
  const char* part = nullptr;
  if (who == kWho6050) part = "MPU-6050";
  else if (who == kWho6500) part = "MPU-6500";
  else if (who == kWho9250) part = "MPU-9250";
  if (!part) {
    ESP_LOGW(TAG,
             "I2C device answered WHO_AM_I 0x%02X, which is none of the parts "
             "this driver knows — motion is off",
             who);
    return ESP_OK;
  }

  // Out of sleep, which is where it powers up. Clocked from the gyro's X PLL
  // rather than the internal oscillator: it is the manufacturer's advice and
  // costs nothing, since the gyro is running either way.
  ESP_RETURN_ON_ERROR(mpu_write(kRegPwrMgmt1, 0x01), TAG, "wake");
  // DLPF at 44 Hz. The panel is a light source bolted to a desk that people
  // type on, and without this the accelerometer faithfully reports the
  // keyboard.
  ESP_RETURN_ON_ERROR(mpu_write(kRegConfig, 0x03), TAG, "dlpf");
  // 1 kHz / (1 + 9) = 100 Hz, which is the render loop's rate. Sampling faster
  // than it is read only means reading stale numbers from a deeper queue.
  ESP_RETURN_ON_ERROR(mpu_write(kRegSmplrtDiv, 9), TAG, "rate");
  ESP_RETURN_ON_ERROR(mpu_write(kRegAccelConfig, 0x08), TAG, "range");
  if (who != kWho6050) {
    // 41 Hz on the accelerometer's own filter. Without this the CONFIG write
    // above filters only the gyro, and the acceleration this actually reads
    // comes through unfiltered at 1 kHz — which on a panel bolted to a desk
    // people type on is a steady supply of knocks that never happened.
    ESP_RETURN_ON_ERROR(mpu_write(kRegAccelConfig2, 0x03), TAG, "accel dlpf");
  }

  g_mpu_ok = true;
  ESP_LOGI(TAG, "%s ready at 0x%02X, +/-4 g at 100 Hz", part, kMpuAddr);
  return ESP_OK;
}

// ------------------------------------------------------------- settings
//
// One blob under one key. Forty separate keys would mean forty defaulted reads
// and a migration every time a field is added; one versioned struct means one
// atomic write and a version check. ui::Settings::migrate owns that decision.
constexpr const char* kNvsNamespace = "pixelbar";
constexpr const char* kNvsKey = "cfg";
bool g_nvs_ready = false;

}  // namespace

esp_err_t init() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_LOGW(TAG, "nvs needs erasing, doing it");
    ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs erase");
    err = nvs_flash_init();
  }
  ESP_RETURN_ON_ERROR(err, TAG, "nvs init");
  g_nvs_ready = true;

  ESP_RETURN_ON_ERROR(init_touch(), TAG, "touch");
  ESP_RETURN_ON_ERROR(init_encoder(), TAG, "encoder");
  ESP_RETURN_ON_ERROR(init_motion(), TAG, "motion");

  // Every input pin, as it rests, once. This is the line to read first when the
  // panel does something nobody asked for.
  ESP_LOGI(TAG, "fitted: touch=%d encoder=%d switch=%d motion=%d",
           pins::kTouchFitted, pins::kEncoderFitted, pins::kEncoderSwitchFitted,
           pins::kMotionFitted);


  ESP_LOGI(TAG, "resting levels: pads L=%d M=%d R=%d, enc A=%d B=%d sw=%d",
           gpio_get_level(pins::kTouchLeft), gpio_get_level(pins::kTouchMiddle),
           gpio_get_level(pins::kTouchRight), gpio_get_level(pins::kEncoderA),
           gpio_get_level(pins::kEncoderB), gpio_get_level(pins::kEncoderSw));
  return ESP_OK;
}

// ------------------------------------------------------------- pin scan

namespace {

// Every GPIO on the SuperMini that is brought out and not already spoken for by
// the LED data line or the USB pins (18 and 19).
const gpio_num_t kScanPins[] = {GPIO_NUM_0, GPIO_NUM_1, GPIO_NUM_2,  GPIO_NUM_3,
                                GPIO_NUM_4, GPIO_NUM_5, GPIO_NUM_6,  GPIO_NUM_7,
                                GPIO_NUM_8, GPIO_NUM_9, GPIO_NUM_20, GPIO_NUM_21};
constexpr int kScanCount = static_cast<int>(sizeof(kScanPins) / sizeof(kScanPins[0]));

bool g_scan_on = false;
uint8_t g_scan_last[kScanCount] = {0};
uint16_t g_scan_edges[kScanCount] = {0};

}  // namespace

void scan_begin() {
  // Reads, and configures nothing.
  //
  // It used to reconfigure every pin it watched, which made it a diagnostic
  // that broke what it measured: a zeroed gpio_config_t means
  // GPIO_INTR_DISABLE, so switching the scan on quietly turned off the
  // encoder's own edge interrupt, and pull_up_en overrode the pull-downs the
  // touch pads need. The one thing this must never do is change the behaviour
  // being investigated.
  //
  // Everything here is already configured by init() — the pads, the encoder,
  // the I2C bus — except GPIO1, 2, 8 and 9, which nothing owns. GPIO2, 8 and 9
  // are strapping pins and are left alone on principle; GPIO1 is the only one
  // that needs an input stage of its own.
  gpio_config_t c = {};
  c.pin_bit_mask = 1ULL << GPIO_NUM_1;
  c.mode = GPIO_MODE_INPUT;
  c.pull_up_en = GPIO_PULLUP_ENABLE;
  c.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&c);

  for (int i = 0; i < kScanCount; ++i) {
    g_scan_last[i] = gpio_get_level(kScanPins[i]) ? 1 : 0;
    g_scan_edges[i] = 0;
  }
  g_scan_on = true;
}
void scan_poll() {
  if (!g_scan_on) return;
  for (int i = 0; i < kScanCount; ++i) {
    const uint8_t now = gpio_get_level(kScanPins[i]) ? 1 : 0;
    if (now != g_scan_last[i]) {
      g_scan_last[i] = now;
      if (g_scan_edges[i] < 60000) ++g_scan_edges[i];
    }
  }
}

void scan_report(char* out, int cap) {
  int n = 0;
  for (int i = 0; i < kScanCount && n < cap - 16; ++i) {
    n += snprintf(out + n, cap - n, "%d:%d/%u ", static_cast<int>(kScanPins[i]),
                  g_scan_last[i], static_cast<unsigned>(g_scan_edges[i]));
  }
  if (n < cap) out[n] = 0;
}

void DevicePorts::advance(float dt_s) {
  uptime_s_ += dt_s;
  pads_.advance(dt_s);
  if (virtual_press_s_ > 0.0f) {
    virtual_press_s_ -= dt_s;
    if (virtual_press_s_ < 0.0f) virtual_press_s_ = 0.0f;
  }
}

void DevicePorts::nudge_encoder(int detents) { virtual_detents_ += detents; }
void DevicePorts::press_switch(float seconds) { virtual_press_s_ = seconds; }

// A pad that has been held far longer than a person holds one.
//
// This began as a check at startup, and startup is the one moment it cannot be
// made at: a TTP223 calibrates itself against its surroundings when it powers
// up, and its output is not meaningful until that finishes about half a second
// later. Sampling at 255 ms reported the right-hand pad as stuck on a board
// whose pads were all fine — a diagnostic that cries wolf is worse than none,
// because the next real one gets ignored.
//
// Duration is the honest signal instead. Nobody rests a finger on a panel for
// half a minute, so a pad that has been high that long is a fault: an
// active-low jumper, a latched toggle-mode module, an output tied to 3V3. It
// catches the same faults the boot check was meant to, plus the latching one
// it could not see, and it cannot be fooled by a module that has not settled.
//
// Said once per episode. A stuck pad is stuck for as long as the panel is
// powered, and a warning every frame would bury everything else in the log.
void DevicePorts::watch_stuck_pads(const bool* touch) {
  static const char* kNames[ui::kZones] = {"left", "middle", "right"};
  const int64_t now = esp_timer_get_time();
  for (int i = 0; i < ui::kZones; ++i) {
    if (!touch[i]) {
      pad_since_us_[i] = 0;
      pad_warned_[i] = false;
      continue;
    }
    if (pad_since_us_[i] == 0) pad_since_us_[i] = now;
    if (pad_warned_[i]) continue;
    if (now - pad_since_us_[i] < 30LL * 1000 * 1000) continue;
    pad_warned_[i] = true;
    ESP_LOGW(TAG,
             "touch pad %s has read held for 30 s — check the TTP223 jumpers "
             "(momentary, active high) before the wiring",
             kNames[i]);
  }
}

void DevicePorts::read_raw(ui::RawInput* out) {
  if (pins::kTouchFitted) {
    out->touch[0] = gpio_get_level(pins::kTouchLeft) != 0;
    out->touch[1] = gpio_get_level(pins::kTouchMiddle) != 0;
    out->touch[2] = gpio_get_level(pins::kTouchRight) != 0;
    watch_stuck_pads(out->touch);
  }
  // A web button and a soldered pad are the same thing from here up.
  pads_.apply(out->touch, ui::kZones);

  // Active low: the switch shorts to ground through the internal pull-up.
  out->encoder_sw =
      (pins::kEncoderSwitchFitted && gpio_get_level(pins::kEncoderSw) == 0) ||
      virtual_press_s_ > 0.0f;
  out->encoder_detents =
      (pins::kEncoderFitted ? g_detents : 0) + virtual_detents_;

  // Saying nothing is better than reporting a plausible stationary reading:
  // motion_valid false makes the whole motion layer absent rather than wrong.
  if (g_mpu_ok) {
    uint8_t raw[6];
    if (mpu_read(kRegAccelXoutH, raw, sizeof(raw)) == ESP_OK) {
      // Big-endian signed, three axes.
      const int16_t x = static_cast<int16_t>((raw[0] << 8) | raw[1]);
      const int16_t y = static_cast<int16_t>((raw[2] << 8) | raw[3]);
      const int16_t z = static_cast<int16_t>((raw[4] << 8) | raw[5]);
      // Straight through, in sensor axes. Which signed axis lies in the panel's
      // plane depends on how the board is mounted in the case, and that is not
      // known until there is a case: the recogniser is written against the
      // magnitude and the in-plane component for exactly that reason, so this
      // mapping can be corrected later without anything above it moving.
      out->ax = static_cast<float>(x) / kAccelLsbPerG;
      out->ay = static_cast<float>(y) / kAccelLsbPerG;
      out->az = static_cast<float>(z) / kAccelLsbPerG;
      out->motion_valid = true;
      last_g_ = std::sqrt(out->ax * out->ax + out->ay * out->ay +
                          out->az * out->az);
      // The component the flat-detection actually uses. Which signed axis lies
      // in the panel's plane depends on how the board ends up mounted, and
      // getting it backwards would have the panel decide it was lying down
      // while standing upright — then sleep, a second later, for no visible
      // reason. Printing it is how that gets caught before a case exists.
      last_plane_ = std::sqrt(out->ax * out->ax + out->ay * out->ay);
    } else {
      // A sensor that stops answering mid-run goes absent rather than frozen.
      // A held reading would be taken for "laid flat" and put the panel to
      // sleep, which is the worst way to find out a wire came off.
      out->motion_valid = false;
      last_g_ = -1.0f;
      last_plane_ = -1.0f;
    }
  } else {
    out->motion_valid = false;
  }
}

uint8_t DevicePorts::raw_pads() const {
  return static_cast<uint8_t>((gpio_get_level(pins::kTouchLeft) ? 1 : 0) |
                              (gpio_get_level(pins::kTouchMiddle) ? 2 : 0) |
                              (gpio_get_level(pins::kTouchRight) ? 4 : 0));
}

uint8_t DevicePorts::raw_encoder() const {
  return static_cast<uint8_t>((gpio_get_level(pins::kEncoderA) ? 1 : 0) |
                              (gpio_get_level(pins::kEncoderB) ? 2 : 0) |
                              (gpio_get_level(pins::kEncoderSw) ? 4 : 0));
}

int32_t DevicePorts::encoder_detents() const { return g_detents; }
uint32_t DevicePorts::encoder_illegal() const { return g_illegal; }

bool DevicePorts::load_settings(ui::Settings* out) {
  if (!g_nvs_ready || !out) return false;
  nvs_handle_t h;
  if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) return false;
  ui::Settings s;
  size_t len = sizeof(s);
  const esp_err_t err = nvs_get_blob(h, kNvsKey, &s, &len);
  nvs_close(h);
  // A blob of the wrong size is from a different build of the struct, and
  // reading it would be reading someone else's field layout.
  if (err != ESP_OK || len != sizeof(s)) return false;
  *out = s;
  return true;
}

bool DevicePorts::save_settings(const ui::Settings& s) {
  if (!g_nvs_ready) return false;
  nvs_handle_t h;
  if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) return false;
  esp_err_t err = nvs_set_blob(h, kNvsKey, &s, sizeof(s));
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);
  if (err != ESP_OK) ESP_LOGW(TAG, "settings save failed: %s", esp_err_to_name(err));
  return err == ESP_OK;
}

bool DevicePorts::wall_clock(int* h, int* m, int* s) {
  // Returning false is what makes the Clock screen show `--:--` instead of a
  // plausible lie. Before the first SNTP sync the system clock reads 1970, and
  // drawing that as 05:30 would be a clock that is confidently wrong.
  if (!time_valid_) return false;
  const time_t now = time(NULL);
  struct tm tm;
  localtime_r(&now, &tm);
  *h = tm.tm_hour;
  *m = tm.tm_min;
  *s = tm.tm_sec;
  return true;
}

bool DevicePorts::wifi_connected() { return wifi_; }
int64_t DevicePorts::unix_time() {
  // Zero until SNTP has answered, for the same reason wall_clock returns false
  // then: before the first sync the system clock reads 1970, and a countdown
  // against that is not a countdown.
  if (!time_valid_) return 0;
  return (int64_t)time(NULL);
}

bool DevicePorts::take_draw(ui::DrawPayload* out) {
  net_draw_t d;
  if (!net_take_draw(&d)) return false;
  // Field by field rather than a memcpy: the two structs are deliberately
  // separate types on either side of a seam that `ui` must not see through,
  // and a memcpy would silently start lying the moment one of them changed.
  snprintf(out->text, sizeof(out->text), "%s", d.text);
  snprintf(out->icon, sizeof(out->icon), "%s", d.icon);
  snprintf(out->source, sizeof(out->source), "%s", d.source);
  out->priority = d.priority;
  out->ttl_s = d.ttl_s;
  out->until_unix = d.until_unix;
  out->bar = d.bar;
  out->tint = d.tint;
  return true;
}

ui::Ports::NetMode DevicePorts::net_mode() { return net_mode_; }
const char* DevicePorts::net_text() { return net_text_; }
// The global, not this method. Without the scope it calls itself forever, and
// the compiler is perfectly happy with that.
const char* DevicePorts::net_error() { return ::net_error(); }

// Straight through to the network component. These run on the render loop, not
// on the server's task, which is the one difference from every other path into
// `net` — and the reason they are plain calls rather than queued commands is
// that they do not touch the model at all.
int DevicePorts::paired_count() { return net_paired_count(); }
const char* DevicePorts::paired_name(int index) { return net_paired_name(index); }
void DevicePorts::begin_pairing() { net_begin_pairing(); }
void DevicePorts::forget_hosts() { net_forget_hosts(); }
void DevicePorts::forget_network() { net_forget_network(); }
void DevicePorts::factory_reset() { net_factory_reset(); }
bool DevicePorts::wifi_enabled() { return net_wifi_enabled(); }
void DevicePorts::set_wifi_enabled(bool on) { net_wifi_set_enabled(on); }

}  // namespace board
