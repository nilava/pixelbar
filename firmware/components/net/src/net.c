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

// Maximum transmit power, in quarter-dBm units: 34 is 8.5 dBm. The long
// comment at the call site explains why this is not the hardware maximum.
static const int8_t kTxPowerQuarterDbm = 34;

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
static bool s_scanning = false;

static void retry_connect(void* arg) { esp_wifi_connect(); }

// ------------------------------------------------------------------ wifi

// Lists what the radio can actually see, with the auth mode of each, because
// "the network is not there", "the password is wrong" and "the access point
// wants protected management frames" all look identical from the panel and are
// entirely different problems.
//
// On its own task, which then exits. A blocking scan cannot run on the event
// task — that is where the disconnect handler lives, and calling it from there
// crash-looped the board — and it has no business on the timer task either.
static void scan_and_report(void) {
  wifi_scan_config_t sc = {0};
  sc.show_hidden = false;
  if (esp_wifi_scan_start(&sc, true) != ESP_OK) return;
  uint16_t n = 0;
  esp_wifi_scan_get_ap_num(&n);
  if (n > 12) n = 12;
  wifi_ap_record_t recs[12];
  if (esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK) return;
  ESP_LOGW(TAG, "visible 2.4 GHz networks (%u):", n);
  for (uint16_t i = 0; i < n; ++i) {
    // The auth mode is the thing worth knowing when the password is right and
    // the join still fails: WPA3-only and mesh networks that require protected
    // management frames both reject a station that has not asked for them.
    const char* auth = "?";
    switch (recs[i].authmode) {
      case WIFI_AUTH_OPEN: auth = "open"; break;
      case WIFI_AUTH_WEP: auth = "WEP"; break;
      case WIFI_AUTH_WPA_PSK: auth = "WPA"; break;
      case WIFI_AUTH_WPA2_PSK: auth = "WPA2"; break;
      case WIFI_AUTH_WPA_WPA2_PSK: auth = "WPA/WPA2"; break;
      case WIFI_AUTH_ENTERPRISE: auth = "enterprise"; break;
      case WIFI_AUTH_WPA3_PSK: auth = "WPA3"; break;
      case WIFI_AUTH_WPA2_WPA3_PSK: auth = "WPA2/WPA3"; break;
      case WIFI_AUTH_WAPI_PSK: auth = "WAPI"; break;
      case WIFI_AUTH_OWE: auth = "OWE"; break;
      default: break;
    }
    ESP_LOGW(TAG, "  %-20s ch%-3d %4d dBm  %-10s %02x:%02x:%02x:%02x:%02x:%02x",
             (const char*)recs[i].ssid, recs[i].primary, recs[i].rssi, auth,
             recs[i].bssid[0], recs[i].bssid[1], recs[i].bssid[2],
             recs[i].bssid[3], recs[i].bssid[4], recs[i].bssid[5]);
  }
}

static void on_wifi(void* arg, esp_event_base_t base, int32_t id, void* data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    s_connected = false;
    s_ip[0] = 0;
    // The reason is the whole diagnosis and costs one line. 201 is "no such
    // network" — which on this chip most often means the network is 5 GHz,
    // because the C3 has a 2.4 GHz radio and nothing else. 15 and 2 are the
    // password being wrong.
    const wifi_event_sta_disconnected_t* d =
        (const wifi_event_sta_disconnected_t*)data;
    const uint8_t why = d ? d->reason : 0;
    const char* name = "?";
    switch (why) {
      case WIFI_REASON_NO_AP_FOUND: name = "no such network (2.4 GHz only?)"; break;
      case WIFI_REASON_AUTH_FAIL: name = "auth failed"; break;
      case WIFI_REASON_AUTH_EXPIRE: name = "auth expired"; break;
      case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: name = "wrong password"; break;
      case WIFI_REASON_HANDSHAKE_TIMEOUT: name = "handshake timeout"; break;
      case WIFI_REASON_CONNECTION_FAIL: name = "connection failed"; break;
      case WIFI_REASON_ASSOC_LEAVE: name = "left"; break;
      default: break;
    }
    // Backoff, because a tight reconnect loop is a burst of radio work and
    // long interrupt-disabled windows several times a second, while the panel
    // is drawing the whole time.
    //
    // Scheduled on a timer rather than slept for here. This runs on the shared
    // system event task, so sleeping in it would stall every other event in the
    // firmware — including the one that reports the address when a later
    // attempt succeeds.
    if (s_scanning) return;  // the disconnect below is ours, not a failure
    const int delay_ms = (s_retries < 6) ? (250 << s_retries) : 30000;
    if (s_retries < 6) ++s_retries;
    ESP_LOGW(TAG, "disconnected: reason %u, %s. retrying in %d ms", why, name,
             delay_ms);
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

static void scan_task(void* arg) {
  // Late enough that a working network will have joined and this never runs.
  vTaskDelay(pdMS_TO_TICKS(6000));
  if (!s_connected) {
    // A scan cannot run while a join is in flight — it returns nothing — so
    // stand the retries down first and put them back afterwards.
    if (s_retry_timer) esp_timer_stop(s_retry_timer);
    s_scanning = true;
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(300));
    scan_and_report();
    s_scanning = false;
    esp_wifi_connect();
  }
  vTaskDelete(NULL);
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

  // In RAM, not NVS: the credentials come from the build, so there is nothing
  // to persist, and it keeps the radio's state clear of whatever the previous
  // firmware on this board left in that partition.
  ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "storage");
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
  // Deliberately nothing else.
  //
  // PMF, SAE key derivation, an auth-mode floor, all-channel scanning and a
  // sort by signal were all added while chasing this, each a plausible fix for
  // a plausible cause, and none of them were the cause. Settings added on top
  // of a failure are how a configuration becomes a haystack. WLED joins this
  // network from this board with nothing but an SSID and a password, so that is
  // what this does too; anything beyond it has to earn its place by fixing
  // something that is actually broken.
  ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");
  ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wc), TAG, "config");
  ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");
  // After start, not before: the radio has to exist before its power policy
  // means anything.
  esp_wifi_set_ps(WIFI_PS_NONE);

  // Transmit power, and it is not a tuning knob — it is the fix for the bug
  // that held this component up for an evening.
  //
  // At the default 20 dBm this board could not associate with anything. The
  // 802.11 authentication frame went unanswered and timed out after exactly
  // 1000 ms, reason 2, on a home router and on a phone hotspot alike, while
  // scans saw that same network at -32 dBm. Receive perfect, transmit never
  // heard. The IDF's own station example failed identically, which is what
  // ruled out every line of our code.
  //
  // The cause is this board rather than this firmware. The ESP32-C3 SuperMini
  // pairs a poorly matched onboard antenna with a small LDO, and a full-power
  // transmit burst pulls a few hundred milliamps for the length of a frame.
  // The rail sags, the PA output is garbage, and the access point hears
  // nothing at all — which looks like a dead transmitter and is really a
  // collapsing supply. Backing off to 8.5 dBm keeps the burst inside what the
  // regulator can deliver, and the same authentication frame is answered in
  // 10 ms instead of never.
  //
  // Cost: a few metres of range on a device that sits on a desk. Do not raise
  // this without re-running the join on a cold board.
  esp_wifi_set_max_tx_power(kTxPowerQuarterDbm);
  int8_t tx = 0;
  esp_wifi_get_max_tx_power(&tx);
  ESP_LOGI(TAG, "tx power %d (%.1f dBm)", tx, tx / 4.0f);
  // The station's MAC. When a plain WPA2 join keeps failing with a correct
  // password and a strong signal, the next suspect is the access point
  // refusing this particular client — an allow-list, a client limit, or band
  // steering pushing a 2.4 GHz-only radio at a 5 GHz band it cannot reach. All
  // three are checked from the router, and all three need this number.
  uint8_t mac[6] = {0};
  esp_wifi_get_mac(WIFI_IF_STA, mac);
  ESP_LOGI(TAG, "station MAC %02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1],
           mac[2], mac[3], mac[4], mac[5]);

  ESP_LOGI(TAG, "joining %s", PIXELBAR_WIFI_SSID);
  xTaskCreate(scan_task, "wifi_scan", 3072, NULL, 3, NULL);
  return ESP_OK;
}

bool net_connected(void) { return s_connected; }
const char* net_ip(void) { return s_ip; }
void net_publish(const net_status_t* s) { s_status = *s; }

bool net_take_cmd(net_cmd_t* out) {
  if (!s_cmds) return false;
  return xQueueReceive(s_cmds, out, 0) == pdTRUE;
}
