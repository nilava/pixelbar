#include "net/net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "secrets.h"

static const char* TAG = "net";

// The page, gzipped at build time and linked in. There is no filesystem
// partition to put it in and there is not going to be: partitions.csv already
// spends 3.5 MB of a 4 MB part on two OTA slots, and moving those would mean a
// wired reflash of anything already in the field.
extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[] asm("_binary_index_html_gz_end");

static httpd_handle_t s_server = NULL;
static QueueHandle_t s_cmds = NULL;
static net_status_t s_status;
static bool s_connected = false;
static char s_ip[16] = {0};
static int s_retries = 0;
static esp_timer_handle_t s_retry_timer = NULL;

static void retry_connect(void* arg) { esp_wifi_connect(); }

// ------------------------------------------------------------------ wifi

static void on_wifi(void* arg, esp_event_base_t base, int32_t id, void* data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    s_connected = false;
    s_ip[0] = 0;
    // Backoff, because a tight reconnect loop is a burst of radio work and
    // long interrupt-disabled windows several times a second, while the panel
    // is drawing the whole time.
    //
    // Scheduled on a timer rather than slept for here. This runs on the shared
    // system event task, so sleeping in it would stall every other event in the
    // firmware — including the one that reports the address when a later
    // attempt succeeds.
    const int delay_ms = (s_retries < 6) ? (250 << s_retries) : 30000;
    if (s_retries < 6) ++s_retries;
    ESP_LOGW(TAG, "disconnected, retrying in %d ms", delay_ms);
    if (s_retry_timer) {
      esp_timer_stop(s_retry_timer);
      esp_timer_start_once(s_retry_timer, (uint64_t)delay_ms * 1000);
    }
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    const ip_event_got_ip_t* e = (const ip_event_got_ip_t*)data;
    snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
    s_connected = true;
    s_retries = 0;
    ESP_LOGI(TAG, "connected: http://%s", s_ip);
  }
}

// ------------------------------------------------------------------ api

static bool body_of(httpd_req_t* r, char* buf, size_t cap) {
  const size_t n = r->content_len < cap - 1 ? r->content_len : cap - 1;
  if (n == 0) {
    buf[0] = 0;
    return true;
  }
  int got = httpd_req_recv(r, buf, n);
  if (got <= 0) return false;
  buf[got] = 0;
  return true;
}

// Small enough that a parser would be more code than it saves, and the input is
// our own page. Finds "key": and reads the number after it.
static bool field(const char* body, const char* key, int* out) {
  const char* p = strstr(body, key);
  if (!p) return false;
  p = strchr(p, ':');
  if (!p) return false;
  *out = atoi(p + 1);
  return true;
}

static void push(uint8_t kind, int16_t a) {
  if (!s_cmds) return;
  const net_cmd_t c = {kind, a};
  xQueueSend(s_cmds, &c, 0);  // never block the server on the render loop
}

static esp_err_t get_root(httpd_req_t* r) {
  httpd_resp_set_type(r, "text/html");
  httpd_resp_set_hdr(r, "Content-Encoding", "gzip");
  return httpd_resp_send(r, (const char*)index_html_gz_start,
                         index_html_gz_end - index_html_gz_start);
}

static esp_err_t get_state(httpd_req_t* r) {
  char buf[320];
  const int n = snprintf(
      buf, sizeof(buf),
      "{\"screen\":\"%s\",\"status\":\"%s\",\"brightness\":%u,"
      "\"timer_left\":%d,\"timer_running\":%s,\"fps\":%.1f,"
      "\"detents\":%ld,\"illegal\":%lu,\"ip\":\"%s\"}",
      s_status.screen, s_status.status, s_status.brightness, s_status.timer_left_s,
      s_status.timer_running ? "true" : "false", s_status.fps,
      (long)s_status.detents, (unsigned long)s_status.illegal, s_ip);
  httpd_resp_set_type(r, "application/json");
  return httpd_resp_send(r, buf, n);
}

static esp_err_t post_input(httpd_req_t* r) {
  char body[192];
  if (!body_of(r, body, sizeof(body))) return httpd_resp_send_500(r);

  int v = 0;
  if (field(body, "\"tap\"", &v)) push(NET_CMD_TAP, (int16_t)v);
  if (field(body, "\"hold\"", &v)) push(NET_CMD_HOLD, (int16_t)v);
  if (field(body, "\"release\"", &v)) push(NET_CMD_RELEASE, (int16_t)v);
  if (field(body, "\"swipe\"", &v)) push(NET_CMD_SWIPE, (int16_t)v);
  if (field(body, "\"turn\"", &v)) push(NET_CMD_TURN, (int16_t)v);
  if (field(body, "\"press\"", &v)) push(NET_CMD_PRESS, 0);
  if (field(body, "\"presshold\"", &v)) push(NET_CMD_PRESS_HOLD, 0);
  if (field(body, "\"status\"", &v)) push(NET_CMD_STATUS, (int16_t)v);
  if (field(body, "\"brightness\"", &v)) push(NET_CMD_BRIGHTNESS, (int16_t)v);

  httpd_resp_set_type(r, "application/json");
  return httpd_resp_send(r, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t start_server(void) {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.max_open_sockets = 4;   // fewer sockets, less RAM; nothing needs more
  cfg.lru_purge_enable = true;  // a stuck client cannot lock the panel out
  cfg.max_uri_handlers = 8;
  cfg.stack_size = 4096;
  ESP_RETURN_ON_ERROR(httpd_start(&s_server, &cfg), TAG, "httpd");

  const httpd_uri_t root = {"/", HTTP_GET, get_root, NULL};
  const httpd_uri_t state = {"/api/state", HTTP_GET, get_state, NULL};
  const httpd_uri_t input = {"/api/input", HTTP_POST, post_input, NULL};
  httpd_register_uri_handler(s_server, &root);
  httpd_register_uri_handler(s_server, &state);
  httpd_register_uri_handler(s_server, &input);
  return ESP_OK;
}

// The server is started from the event that brings the address, on the event
// task, so app_main never waits for a network to come up.
static void on_got_ip(void* arg, esp_event_base_t base, int32_t id, void* data) {
  if (!s_server) {
    if (start_server() == ESP_OK) ESP_LOGI(TAG, "web ui on http://%s", s_ip);
  }
}

esp_err_t net_start(void) {
  s_cmds = xQueueCreate(16, sizeof(net_cmd_t));
  if (!s_cmds) return ESP_ERR_NO_MEM;
  memset(&s_status, 0, sizeof(s_status));

  const esp_timer_create_args_t targs = {.callback = retry_connect,
                                         .name = "wifi_retry"};
  ESP_RETURN_ON_ERROR(esp_timer_create(&targs, &s_retry_timer), TAG, "retry timer");

  ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
  ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
  ESP_RETURN_ON_ERROR(esp_wifi_init(&ic), TAG, "wifi init");
  ESP_RETURN_ON_ERROR(
      esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi,
                                          NULL, NULL),
      TAG, "wifi events");
  ESP_RETURN_ON_ERROR(
      esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi,
                                          NULL, NULL),
      TAG, "ip event");
  ESP_RETURN_ON_ERROR(
      esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                          on_got_ip, NULL, NULL),
      TAG, "server start");

  wifi_config_t wc = {0};
  strncpy((char*)wc.sta.ssid, PIXELBAR_WIFI_SSID, sizeof(wc.sta.ssid) - 1);
  strncpy((char*)wc.sta.password, PIXELBAR_WIFI_PASS, sizeof(wc.sta.password) - 1);
  ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");
  ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wc), TAG, "config");
  // The radio sleeps between beacons by default, which adds latency to every
  // request for power this device is not short of.
  esp_wifi_set_ps(WIFI_PS_NONE);
  ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");
  ESP_LOGI(TAG, "joining %s", PIXELBAR_WIFI_SSID);
  return ESP_OK;
}

bool net_connected(void) { return s_connected; }
const char* net_ip(void) { return s_ip; }
void net_publish(const net_status_t* s) { s_status = *s; }

bool net_take_cmd(net_cmd_t* out) {
  if (!s_cmds) return false;
  return xQueueReceive(s_cmds, out, 0) == pdTRUE;
}
