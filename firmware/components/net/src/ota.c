// Firmware updates over the network.
//
// A browser upload rather than a pull from a release server. There is no
// release infrastructure to pull from, and an HTTPS client would need a
// certificate bundle of about 64 KB to fetch a file across a home LAN — a bad
// trade for a device two rooms from the laptop that built the image.
//
// The write goes straight from the socket into flash in 4 KB pieces. Buffering
// the whole image first would need 875 KB of a 400 KB heap, so there is no
// version of this that holds the file in memory.
//
// Flash writes disable the instruction cache for milliseconds at a time, and
// the render loop's code lives in flash. That is survivable only because the
// LED output moved to SPI + GDMA: once a frame is queued, DMA clocks it out of
// SRAM with no CPU involvement, so a stalled loop drops frames instead of
// tearing the panel mid-frame. On the RMT path this endpoint would have
// strobed the whole panel for the length of the upload.
#include "ota.h"

#include <string.h>

#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"

static const char* TAG = "ota";

// -1 when idle, otherwise 0..1. Read by the render loop once a frame to decide
// whether the panel shows the progress screen, so it is deliberately a plain
// float: a torn read costs one frame of a progress bar.
static float s_progress = -1.0f;
// Set once the upload has finished, so the reboot happens off the server's
// task rather than inside a handler that still owes a response.
static esp_timer_handle_t s_reboot_timer = NULL;

float ota_progress(void) { return s_progress; }

static void reboot(void* arg) { esp_restart(); }

esp_err_t ota_post(httpd_req_t* r) {
  const esp_partition_t* target = esp_ota_get_next_update_partition(NULL);
  if (!target) {
    httpd_resp_set_status(r, "500 Internal Server Error");
    return httpd_resp_sendstr(r, "{\"error\":\"no ota partition\"}");
  }

  const int total = r->content_len;
  if (total <= 0) {
    httpd_resp_set_status(r, "400 Bad Request");
    return httpd_resp_sendstr(r, "{\"error\":\"no content-length\"}");
  }
  if (total > (int)target->size) {
    httpd_resp_set_status(r, "413 Payload Too Large");
    return httpd_resp_sendstr(r, "{\"error\":\"image larger than the slot\"}");
  }

  ESP_LOGW(TAG, "update starting: %d bytes into %s", total, target->label);

  esp_ota_handle_t handle = 0;
  esp_err_t err = esp_ota_begin(target, total, &handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "begin failed: %s", esp_err_to_name(err));
    httpd_resp_set_status(r, "500 Internal Server Error");
    return httpd_resp_sendstr(r, "{\"error\":\"ota begin failed\"}");
  }

  s_progress = 0.0f;
  static char buf[4096];
  int received = 0;
  bool checked_header = false;

  while (received < total) {
    const int want = (total - received) < (int)sizeof(buf) ? (total - received)
                                                           : (int)sizeof(buf);
    const int n = httpd_req_recv(r, buf, want);
    if (n <= 0) {
      // A dropped connection part way through leaves the *other* slot half
      // written, which is harmless: the boot partition is not switched until
      // the whole image has been verified.
      ESP_LOGE(TAG, "upload aborted after %d of %d bytes", received, total);
      esp_ota_abort(handle);
      s_progress = -1.0f;
      httpd_resp_set_status(r, "400 Bad Request");
      return httpd_resp_sendstr(r, "{\"error\":\"upload interrupted\"}");
    }

    // Check what was sent before writing a megabyte of it. The magic byte is
    // the cheapest way to catch the common mistake of uploading the wrong file
    // — a bootloader, a partition table, or something that is not an image at
    // all — and it costs one comparison.
    if (!checked_header) {
      checked_header = true;
      if (n < 1 || (uint8_t)buf[0] != ESP_IMAGE_HEADER_MAGIC) {
        ESP_LOGE(TAG, "not a firmware image");
        esp_ota_abort(handle);
        s_progress = -1.0f;
        httpd_resp_set_status(r, "400 Bad Request");
        return httpd_resp_sendstr(r, "{\"error\":\"not a firmware image\"}");
      }
    }

    err = esp_ota_write(handle, buf, n);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "write failed: %s", esp_err_to_name(err));
      esp_ota_abort(handle);
      s_progress = -1.0f;
      httpd_resp_set_status(r, "500 Internal Server Error");
      return httpd_resp_sendstr(r, "{\"error\":\"flash write failed\"}");
    }
    received += n;
    s_progress = (float)received / (float)total;
  }

  // Verifies the image before it is allowed to become bootable.
  err = esp_ota_end(handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "verify failed: %s", esp_err_to_name(err));
    s_progress = -1.0f;
    httpd_resp_set_status(r, "400 Bad Request");
    return httpd_resp_sendstr(r, "{\"error\":\"image failed verification\"}");
  }

  err = esp_ota_set_boot_partition(target);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "set boot failed: %s", esp_err_to_name(err));
    s_progress = -1.0f;
    httpd_resp_set_status(r, "500 Internal Server Error");
    return httpd_resp_sendstr(r, "{\"error\":\"could not switch slot\"}");
  }

  ESP_LOGW(TAG, "update written to %s; restarting", target->label);
  httpd_resp_set_type(r, "application/json");
  httpd_resp_sendstr(r, "{\"ok\":true,\"restarting\":true}");

  // Off the server's task, and late enough that the reply has gone out.
  if (!s_reboot_timer) {
    const esp_timer_create_args_t a = {.callback = reboot, .name = "ota_reboot"};
    esp_timer_create(&a, &s_reboot_timer);
  }
  esp_timer_start_once(s_reboot_timer, 700000);
  return ESP_OK;
}

void ota_mark_healthy(void) {
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  if (esp_ota_get_state_partition(running, &state) != ESP_OK) return;
  if (state != ESP_OTA_IMG_PENDING_VERIFY) return;
  // Nothing above this line has committed the new image. Until this call the
  // bootloader will put the old one back on the next reset, which is what
  // makes a bad update recoverable without a cable.
  if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK)
    ESP_LOGW(TAG, "update confirmed healthy; rollback cancelled");
}
