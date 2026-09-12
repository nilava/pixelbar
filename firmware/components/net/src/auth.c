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
#include <time.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char* TAG = "auth";

#define AUTH_NS "pixelbar"
#define AUTH_KEY "clients"

// A token per client, not one token for everybody.
//
// This was a single token minted on first boot and handed to every host that
// ever paired. Fifty browsers would have received fifty copies of the same
// secret, with no record of who held it and no way to withdraw one without
// withdrawing all of them — and no way even to answer "what is paired to this
// device", which is a question the panel is now expected to answer. The
// Bluetooth side has had per-host bonds, enumerable and revocable, all along;
// this is the same idea for the HTTP side.
#define AUTH_MAX_CLIENTS 8
#define AUTH_VERSION 2

typedef struct {
  char token[33];    // 32 hex characters
  char name[20];     // what the host called itself when it paired
  uint32_t issued;   // unix seconds, or 0 if the clock was not set yet
} auth_client_t;

typedef struct {
  uint8_t version;
  uint8_t count;
  auth_client_t c[AUTH_MAX_CLIENTS];
} auth_blob_t;

static auth_blob_t s_clients;

// Stored as issued rather than hashed.
//
// Hashing would stop someone who has read the flash from using the tokens —
// but someone who has read the flash has the WiFi PSK from the next key along
// and a UART, and can write their own firmware. It would defend the weakest
// thing in the box against an attacker who already owns the box. What matters
// is that a token never leaves the device except to the host that earned it,
// so the listing endpoint returns names and never tokens.

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

static void make_token(char out[33]) {
  static const char kHex[] = "0123456789abcdef";
  uint8_t raw[16];
  esp_fill_random(raw, sizeof(raw));
  for (int i = 0; i < 16; ++i) {
    out[i * 2] = kHex[raw[i] >> 4];
    out[i * 2 + 1] = kHex[raw[i] & 0x0F];
  }
  out[32] = '\0';
}

static void save(void) {
  nvs_handle_t h;
  if (nvs_open(AUTH_NS, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_blob(h, AUTH_KEY, &s_clients, sizeof(s_clients));
  nvs_commit(h);
  nvs_close(h);
}

void auth_init(void) {
  nvs_handle_t h;
  size_t len = sizeof(s_clients);
  if (nvs_open(AUTH_NS, NVS_READONLY, &h) == ESP_OK) {
    const esp_err_t err = nvs_get_blob(h, AUTH_KEY, &s_clients, &len);
    nvs_close(h);
    if (err == ESP_OK && len == sizeof(s_clients) &&
        s_clients.version == AUTH_VERSION) {
      if (s_clients.count > AUTH_MAX_CLIENTS) s_clients.count = AUTH_MAX_CLIENTS;
      ESP_LOGI(TAG, "%u paired client%s", s_clients.count,
               s_clients.count == 1 ? "" : "s");
      return;
    }
  }
  // Nothing stored, or a blob from the single-token era. Start empty rather
  // than trying to carry the old shared token forward: it was held by an
  // unknown number of hosts under no name, so there is nothing to carry that
  // would mean anything in a list. Everything re-pairs once, which is a minute
  // of somebody's time against a permanent answer to "who has access".
  memset(&s_clients, 0, sizeof(s_clients));
  s_clients.version = AUTH_VERSION;
  s_clients.count = 0;
  save();
  ESP_LOGI(TAG, "no paired clients");
}

int auth_client_count(void) { return s_clients.count; }
int auth_client_max(void) { return AUTH_MAX_CLIENTS; }

const char* auth_client_name(int i) {
  if (i < 0 || i >= s_clients.count) return "";
  return s_clients.c[i].name;
}

uint32_t auth_client_issued(int i) {
  if (i < 0 || i >= s_clients.count) return 0;
  return s_clients.c[i].issued;
}

bool auth_revoke(int i) {
  if (i < 0 || i >= s_clients.count) return false;
  ESP_LOGW(TAG, "revoked \"%s\"", s_clients.c[i].name);
  for (int j = i; j + 1 < s_clients.count; ++j) s_clients.c[j] = s_clients.c[j + 1];
  --s_clients.count;
  memset(&s_clients.c[s_clients.count], 0, sizeof(auth_client_t));
  save();
  return true;
}

void auth_revoke_all(void) {
  ESP_LOGW(TAG, "revoked all %u clients", s_clients.count);
  memset(&s_clients, 0, sizeof(s_clients));
  s_clients.version = AUTH_VERSION;
  save();
}

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

bool auth_redeem(uint32_t code, const char* name, const char** token_out) {
  if (auth_pairing_code() == 0) return false;
  if (++s_attempts > PAIR_MAX_ATTEMPTS) {
    s_code = 0;
    ESP_LOGW(TAG, "too many attempts; pairing window closed");
    return false;
  }
  if (code != s_code) return false;
  if (s_clients.count >= AUTH_MAX_CLIENTS) {
    // Refused rather than evicting the oldest. Silently dropping a host that
    // still works, to make room for one that has only just asked, is a way to
    // make a device look broken to whoever was using the one you dropped.
    s_code = 0;
    ESP_LOGW(TAG, "no room: %d clients already paired", AUTH_MAX_CLIENTS);
    return false;
  }
  // Spent. A code that keeps working is a password, and this is not one.
  s_code = 0;

  auth_client_t* c = &s_clients.c[s_clients.count++];
  memset(c, 0, sizeof(*c));
  make_token(c->token);
  snprintf(c->name, sizeof(c->name), "%s", (name && name[0]) ? name : "host");
  const time_t now = time(NULL);
  c->issued = (now > 1600000000) ? (uint32_t)now : 0;  // 0 until the clock is set
  save();

  *token_out = c->token;
  ESP_LOGI(TAG, "paired \"%s\" (%u of %d)", c->name, s_clients.count, AUTH_MAX_CLIENTS);
  return true;
}

bool auth_full(void) { return s_clients.count >= AUTH_MAX_CLIENTS; }

bool auth_ok(const char* presented) {
  if (!presented) return false;
  bool any = false;
  // Every client is checked even after a match, so the time taken does not
  // depend on which one it was — the same reason the comparison below is
  // constant time. Eight of them is nothing.
  for (int i = 0; i < s_clients.count; ++i) {
    uint8_t diff = 0;
    const char* t = s_clients.c[i].token;
    for (int k = 0; k < 32; ++k) {
      const char b = presented[k];
      diff |= (uint8_t)(t[k] ^ b);
      if (b == '\0') { diff |= 1; break; }
    }
    if (diff == 0 && presented[32] == '\0') any = true;
  }
  return any;
}
