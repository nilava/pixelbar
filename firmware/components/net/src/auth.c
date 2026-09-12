// Who is allowed to change things.
//
// Bluetooth pairs with a six-digit code on the panel and refuses a command
// without it. HTTP accepted anything from anyone on the LAN — including a
// firmware image, which is arbitrary code execution on the device, and a
// request to forget the network. That asymmetry was the thing worth closing:
// the weaker door is the one that matters.
//
// Deliberately a token over plain HTTP rather than TLS. This is a gadget on a
// home LAN; a certificate store, a clock that must be right before anything
// works, and a 64 KB bundle would buy protection against an attacker who is
// already inside the network and could unplug it instead. What a token stops
// is the casual and the accidental — a script pointed at the wrong address, a
// housemate who found the web page — and that is the actual threat to a desk
// ornament.
#include "auth.h"

#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char* TAG = "auth";

#define AUTH_NS "pixelbar"
#define AUTH_KEY "token"

// Sixteen bytes as thirty-two hex characters. Long enough that guessing is not
// a strategy, short enough to be typed once if it ever has to be.
static char s_token[33] = {0};

// The pairing window: a code on the panel, and how long it stays worth typing.
static uint32_t s_code = 0;
static int64_t s_code_until_us = 0;
// Sixty seconds is long enough to walk to the panel and read it, short enough
// that a code left on screen by a forgotten request is not a standing offer.
#define PAIR_WINDOW_US (60 * 1000000LL)
// A wrong code costs the whole window. Six digits is a million possibilities
// and a minute is not long enough to try many, but "not long enough" is a
// worse argument than "you get one go".
static int s_attempts = 0;
#define PAIR_MAX_ATTEMPTS 3

static void make_token(void) {
  static const char kHex[] = "0123456789abcdef";
  uint8_t raw[16];
  esp_fill_random(raw, sizeof(raw));
  for (int i = 0; i < 16; ++i) {
    s_token[i * 2] = kHex[raw[i] >> 4];
    s_token[i * 2 + 1] = kHex[raw[i] & 0x0F];
  }
  s_token[32] = '\0';
}

void auth_init(void) {
  nvs_handle_t h;
  size_t len = sizeof(s_token);
  if (nvs_open(AUTH_NS, NVS_READONLY, &h) == ESP_OK) {
    const esp_err_t err = nvs_get_str(h, AUTH_KEY, s_token, &len);
    nvs_close(h);
    if (err == ESP_OK && strlen(s_token) == 32) return;
  }

  make_token();
  if (nvs_open(AUTH_NS, NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_str(h, AUTH_KEY, s_token);
    nvs_commit(h);
    nvs_close(h);
  }
  // Never logged. It is the one secret this device holds, and a serial log is
  // read over a shoulder more often than a network is sniffed.
  ESP_LOGI(TAG, "issued a new API token");
}

const char* auth_token(void) { return s_token; }

uint32_t auth_begin_pairing(void) {
  s_code = esp_random() % 1000000u;
  if (s_code == 0) s_code = 1;  // 0 means "no window open"
  s_code_until_us = esp_timer_get_time() + PAIR_WINDOW_US;
  s_attempts = 0;
  ESP_LOGI(TAG, "pairing window open");
  return s_code;
}

uint32_t auth_pairing_code(void) {
  if (s_code == 0) return 0;
  if (esp_timer_get_time() > s_code_until_us) {
    s_code = 0;
    return 0;
  }
  return s_code;
}

bool auth_redeem(uint32_t code, const char** token_out) {
  if (auth_pairing_code() == 0) return false;
  if (++s_attempts > PAIR_MAX_ATTEMPTS) {
    s_code = 0;
    ESP_LOGW(TAG, "too many attempts; pairing window closed");
    return false;
  }
  if (code != s_code) return false;
  // Spent. A code that keeps working is a password, and this is not one.
  s_code = 0;
  *token_out = s_token;
  ESP_LOGI(TAG, "paired");
  return true;
}

bool auth_ok(const char* presented) {
  if (!presented) return false;
  if (s_token[0] == '\0') return false;
  // Constant time over the fixed 32 characters. The timing of a string compare
  // on a device answering one request at a time over WiFi is not a realistic
  // channel, and writing the careless version anyway is how the habit is lost.
  uint8_t diff = 0;
  for (int i = 0; i < 32; ++i) {
    const char a = s_token[i];
    const char b = presented[i];
    diff |= (uint8_t)(a ^ b);
    if (b == '\0') return false;
  }
  return diff == 0 && presented[32] == '\0';
}
