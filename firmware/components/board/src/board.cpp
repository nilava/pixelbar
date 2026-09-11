#include "board/board.h"

#include <cstdio>
#include <cstring>
#include <ctime>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
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

// In IRAM, along with everything it touches. An interrupt living in flash
// stalls for the milliseconds the cache is disabled during an NVS commit, and
// on an encoder that is detents silently going missing. Quadrature::update is
// header-inline for exactly this reason.
void IRAM_ATTR encoder_isr(void* /*arg*/) {
  const bool a = gpio_get_level(pins::kEncoderA) != 0;
  const bool b = gpio_get_level(pins::kEncoderB) != 0;
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
  for (int i = 0; i < kScanCount; ++i) {
    // GPIO8 and 9 are strapping pins and GPIO2 is one too; they are read here
    // but never configured, so the scan cannot change how the board boots.
    if (kScanPins[i] == GPIO_NUM_2 || kScanPins[i] == GPIO_NUM_8 ||
        kScanPins[i] == GPIO_NUM_9) {
      continue;
    }
    gpio_config_t c = {};
    c.pin_bit_mask = 1ULL << kScanPins[i];
    c.mode = GPIO_MODE_INPUT;
    c.pull_up_en = GPIO_PULLUP_ENABLE;
    c.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&c);
  }
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

void DevicePorts::read_raw(ui::RawInput* out) {
  if (pins::kTouchFitted) {
    out->touch[0] = gpio_get_level(pins::kTouchLeft) != 0;
    out->touch[1] = gpio_get_level(pins::kTouchMiddle) != 0;
    out->touch[2] = gpio_get_level(pins::kTouchRight) != 0;
  }
  // A web button and a soldered pad are the same thing from here up.
  pads_.apply(out->touch, ui::kZones);

  // Active low: the switch shorts to ground through the internal pull-up.
  out->encoder_sw =
      (pins::kEncoderSwitchFitted && gpio_get_level(pins::kEncoderSw) == 0) ||
      virtual_press_s_ > 0.0f;
  out->encoder_detents =
      (pins::kEncoderFitted ? g_detents : 0) + virtual_detents_;

  // The MPU-6050 is not fitted, and saying so is better than reporting a
  // plausible stationary reading: motion_valid false makes the whole motion
  // layer absent rather than wrong.
  out->motion_valid = false;
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
  // No time source yet. Returning false is what makes the Clock screen show
  // `--:--` instead of a plausible lie; it becomes true when SNTP lands.
  (void)h;
  (void)m;
  (void)s;
  return false;
}

bool DevicePorts::wifi_connected() { return wifi_; }

}  // namespace board
