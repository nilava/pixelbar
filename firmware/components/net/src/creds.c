// Where the WiFi credentials live, which is here and nowhere else.
//
// They used to be compiled into the image from a header, which meant the
// device worked on exactly one network and moving it was a wired reflash.
// Worse, a password written into a header does not stay in the header: it
// ends up in the object file, the static library, the .elf and the .bin, so
// an ordinary build tree quietly becomes five more copies of it.
//
// So there is one path in — the setup page — and one place it rests. Nothing
// in the firmware sources contains a credential, which means no build
// artifact can either.
#include "creds.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char* TAG = "creds";

// The same namespace the settings blob uses. One namespace per device, keyed
// by what the value is, rather than a namespace per subsystem.
#define CREDS_NS "pixelbar"
#define CREDS_KEY "wifi"

typedef struct {
  uint8_t version;
  char ssid[33];  // 32 octets plus a terminator
  char pass[65];  // 64 for a raw PSK, plus a terminator
} creds_blob_t;

#define CREDS_VERSION 1

bool creds_load(char* ssid, size_t ssid_cap, char* pass, size_t pass_cap) {
  nvs_handle_t h;
  if (nvs_open(CREDS_NS, NVS_READONLY, &h) != ESP_OK) return false;

  creds_blob_t b;
  size_t len = sizeof(b);
  const esp_err_t err = nvs_get_blob(h, CREDS_KEY, &b, &len);
  nvs_close(h);
  if (err != ESP_OK || len != sizeof(b) || b.version != CREDS_VERSION) return false;

  // A blob that survived but holds an empty SSID is the same as no blob. It is
  // how creds_clear leaves things, and it saves the caller a second question.
  b.ssid[sizeof(b.ssid) - 1] = '\0';
  b.pass[sizeof(b.pass) - 1] = '\0';
  if (b.ssid[0] == '\0') return false;

  strlcpy(ssid, b.ssid, ssid_cap);
  strlcpy(pass, b.pass, pass_cap);
  return true;
}

bool creds_save(const char* ssid, const char* pass) {
  if (!ssid || ssid[0] == '\0') return false;

  creds_blob_t b = {0};
  b.version = CREDS_VERSION;
  strlcpy(b.ssid, ssid, sizeof(b.ssid));
  strlcpy(b.pass, pass ? pass : "", sizeof(b.pass));

  nvs_handle_t h;
  if (nvs_open(CREDS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
  esp_err_t err = nvs_set_blob(h, CREDS_KEY, &b, sizeof(b));
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "save failed: %s", esp_err_to_name(err));
    return false;
  }
  // The SSID, never the password, and not even its length: an SSID is
  // broadcast in the clear and a password is the one thing here worth
  // protecting from anyone reading a serial log over a shoulder.
  ESP_LOGI(TAG, "stored credentials for %s", b.ssid);
  return true;
}

bool creds_clear(void) {
  nvs_handle_t h;
  if (nvs_open(CREDS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
  esp_err_t err = nvs_erase_key(h, CREDS_KEY);
  if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;  // already gone
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);
  return err == ESP_OK;
}
