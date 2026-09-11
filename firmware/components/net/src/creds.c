// Where the WiFi credentials actually live.
//
// Until now they came from secrets.h and were compiled into the image, which
// means the device works on exactly one network and changing it is a wired
// reflash. That is fine for the board on the bench and useless for anything
// else, so NVS is the source of truth from here and the build-time pair is
// demoted to a seed.
//
// The seeding rule matters more than it looks. If NVS is empty and the build
// carries credentials, they are written once and then owned by NVS — so an
// existing board keeps joining the network it already joins, and the very
// next thing the user does from the web page overrides them for good. If the
// seed stayed authoritative it would silently undo every change on the next
// reflash, which is the kind of bug that takes an evening to see.
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
// Set the first time the build's credentials are seeded into NVS, and never
// cleared. Without it, "forget this network" is a no-op with a reboot in the
// middle: the clear empties NVS, the next boot finds it empty, seeds from
// secrets.h again, and rejoins the network the user just asked it to forget.
#define CREDS_SEEDED_KEY "seeded"

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

bool creds_seeded(void) {
  nvs_handle_t h;
  if (nvs_open(CREDS_NS, NVS_READONLY, &h) != ESP_OK) return false;
  uint8_t v = 0;
  const esp_err_t err = nvs_get_u8(h, CREDS_SEEDED_KEY, &v);
  nvs_close(h);
  return err == ESP_OK && v != 0;
}

bool creds_mark_seeded(void) {
  nvs_handle_t h;
  if (nvs_open(CREDS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
  esp_err_t err = nvs_set_u8(h, CREDS_SEEDED_KEY, 1);
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);
  return err == ESP_OK;
}

// Deliberately leaves the seeded marker alone. Clearing it too would put the
// build's credentials back on the next boot, which is the opposite of what
// the user asked for.
bool creds_clear(void) {
  nvs_handle_t h;
  if (nvs_open(CREDS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
  esp_err_t err = nvs_erase_key(h, CREDS_KEY);
  if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;  // already gone
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);
  return err == ESP_OK;
}
