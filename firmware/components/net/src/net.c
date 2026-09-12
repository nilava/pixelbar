#include "net/net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "captive_dns.h"
#include "creds.h"
#include "ble.h"
#include "discovery.h"
#include "ota.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
static const char* TAG = "net";

// Maximum transmit power, in quarter-dBm units: 34 is 8.5 dBm. The long
// comment at the call site explains why this is not the hardware maximum.
static const int8_t kTxPowerQuarterDbm = 34;

// The clock, and why it is allowed to be absent.
//
// Until the first sync there is no time, and the panel says so rather than
// drawing 00:00 — a clock that is confidently wrong is worse than one that
// admits it does not know yet. s_time_valid goes true exactly once, in the
// sync callback, and never goes back: a later failed re-sync leaves the last
// good time on screen, which drifts slowly and is still far better than
// blanking a clock somebody is reading.
static bool s_time_valid = false;

// Asia/Kolkata, in the POSIX form where the sign is inverted: UTC+5:30 is
// written -5:30. Settable from the web page once there is a settings path for
// it; hard-coded until then, because a wrong offset is a wrong clock.
#define PIXELBAR_TZ "IST-5:30"

static void on_time_sync(struct timeval* tv) {
  (void)tv;
  if (!s_time_valid) {
    s_time_valid = true;
    ESP_LOGI(TAG, "clock synchronised");
  }
}

static void start_sntp(void) {
  static bool started = false;
  if (started) return;
  started = true;
  setenv("TZ", PIXELBAR_TZ, 1);
  tzset();
  esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
  cfg.start = true;
  cfg.sync_cb = on_time_sync;
  const esp_err_t err = esp_netif_sntp_init(&cfg);
  if (err != ESP_OK) ESP_LOGW(TAG, "sntp init: %s", esp_err_to_name(err));
}

bool net_time_valid(void) { return s_time_valid; }

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

// ------------------------------------------------------------- provisioning

// What the device is doing about the network, for the panel and the page.
static net_mode_t s_mode = NET_MODE_SETUP;
// The setup network's own name, so the panel can show what to join.
static char s_ap_ssid[24] = {0};

// A join attempted from the setup page, as opposed to the one we do at boot.
// While this is set, a success stores the credentials and a failure is
// reported back to the page rather than retried forever.
static bool s_trying = false;
static char s_try_ssid[33] = {0};
static char s_try_pass[65] = {0};
// What the page polls for. Cleared when a new attempt starts.
static char s_try_error[48] = {0};

// Dropping the setup AP is deferred so the page has a moment to read the new
// address before the network it is asking over disappears underneath it.
static esp_timer_handle_t s_ap_down_timer = NULL;

// How many boot-time join failures before falling back to the setup AP.
//
// Without this a device that moves house, or whose password changes, retries
// an unreachable network forever with no way in: the web page needs the
// network to be reachable, and the panel cannot type a password. Six attempts
// is about twenty seconds with the backoff below.
#define NET_MAX_BOOT_RETRIES 6

// A join the user is watching counts as joining, whatever mode the radio is
// technically in. While the form's credentials are being tried the device is
// still in setup — the AP is up and the portal is serving — but what the
// person standing in front of the panel is doing is waiting to find out
// whether it worked, and that is what the panel should say.
net_mode_t net_mode(void) { return s_trying ? NET_MODE_JOINING : s_mode; }
const char* net_setup_ssid(void) { return s_ap_ssid; }

// The one string the panel shows, chosen by what the device is doing: the
// network to join, the network being tried, or the address to visit. Decided
// here because this is where the state is; main would only be guessing.
const char* net_panel_text(void) {
  if (s_trying) return s_try_ssid;
  if (s_mode == NET_MODE_ONLINE) return s_ip;
  return s_ap_ssid;
}

// The station half has nothing to connect to while the device is in setup
// mode and nobody has submitted the form yet. Calling connect anyway makes it
// hunt for an unconfigured SSID every few seconds — each attempt taking the
// radio off the AP's channel, and filling the log with reason 201 — while a
// phone is trying to hold a connection to the portal.
static bool should_connect(void) {
  return s_mode != NET_MODE_SETUP || s_trying;
}

static void retry_connect(void* arg) {
  if (should_connect()) esp_wifi_connect();
}

// ------------------------------------------------------------ the setup AP

static esp_netif_t* s_ap_netif = NULL;

// Open, and deliberately so.
//
// A password on the setup network would have to be printed on the case or
// shown on a 24-pixel-wide panel, which means it is not a secret; and it would
// have to be typed before the portal could explain itself. What it protects is
// ninety seconds of an isolated network with no route anywhere, on which the
// only thing reachable is a form. The real credential is the one typed into
// that form, and it travels over a link nobody else has joined.
static void start_setup_ap(void) {
  if (s_mode == NET_MODE_SETUP && s_ap_ssid[0]) return;  // already up

  uint8_t mac[6] = {0};
  esp_wifi_get_mac(WIFI_IF_STA, mac);
  snprintf(s_ap_ssid, sizeof(s_ap_ssid), "PIXELBAR-%02X%02X", mac[4], mac[5]);

  if (!s_ap_netif) s_ap_netif = esp_netif_create_default_wifi_ap();

  wifi_config_t ap = {0};
  strlcpy((char*)ap.ap.ssid, s_ap_ssid, sizeof(ap.ap.ssid));
  ap.ap.ssid_len = strlen(s_ap_ssid);
  ap.ap.channel = 1;
  ap.ap.authmode = WIFI_AUTH_OPEN;
  // One at a time. Two phones on a setup network is a mistake, not a use case,
  // and refusing the second is clearer than serving both a form that fights.
  ap.ap.max_connection = 1;

  // APSTA rather than AP: the station half has to stay available so a
  // credential typed into the form can be tried without tearing down the
  // network the form arrived over.
  esp_wifi_set_mode(WIFI_MODE_APSTA);
  esp_wifi_set_config(WIFI_IF_AP, &ap);
  s_mode = NET_MODE_SETUP;

  esp_netif_ip_info_t ip;
  esp_netif_get_ip_info(s_ap_netif, &ip);
  captive_dns_start(ip.ip.addr);
  ESP_LOGI(TAG, "setup network %s, open, at " IPSTR, s_ap_ssid, IP2STR(&ip.ip));
}

// Called on a timer once a join has succeeded, never straight from the event.
static void stop_setup_ap(void* arg) {
  if (s_mode != NET_MODE_ONLINE) return;  // the join went away again
  captive_dns_stop();
  esp_wifi_set_mode(WIFI_MODE_STA);
  s_ap_ssid[0] = '\0';
  ESP_LOGI(TAG, "setup network down");
}

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
    if (should_connect()) esp_wifi_connect();
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

    // A join the user just asked for from the setup page is not retried. They
    // are standing there waiting for an answer, and the answer is that it did
    // not work — almost always a mistyped password, which no amount of
    // retrying will fix. Report it and stay in setup so they can try again.
    if (s_trying) {
      s_trying = false;
      snprintf(s_try_error, sizeof(s_try_error), "%s", name);
      ESP_LOGW(TAG, "setup join failed: reason %u, %s", why, name);
      esp_wifi_set_mode(WIFI_MODE_AP);  // drop the station half, keep the portal
      return;
    }

    // Backoff, because a tight reconnect loop is a burst of radio work and
    // long interrupt-disabled windows several times a second, while the panel
    // is drawing the whole time.
    //
    // Scheduled on a timer rather than slept for here. This runs on the shared
    // system event task, so sleeping in it would stall every other event in the
    // firmware — including the one that reports the address when a later
    // attempt succeeds.
    const int delay_ms = (s_retries < 6) ? (250 << s_retries) : 30000;
    if (s_retries < NET_MAX_BOOT_RETRIES) ++s_retries;
    ESP_LOGW(TAG, "disconnected: reason %u, %s. retrying in %d ms", why, name,
             delay_ms);

    // Out of attempts on the stored network. Rather than retry something
    // unreachable forever — a device that has moved house, or whose password
    // changed — put the setup AP back up so there is a way to fix it. The
    // stored credentials are kept and still retried in the background, so a
    // network that is merely down comes back on its own.
    if (s_mode == NET_MODE_JOINING && s_retries >= NET_MAX_BOOT_RETRIES) {
      ESP_LOGW(TAG, "cannot reach the stored network; opening setup");
      start_setup_ap();
    }

    if (s_retry_timer && should_connect()) {
      esp_timer_stop(s_retry_timer);
      esp_timer_start_once(s_retry_timer, (uint64_t)delay_ms * 1000);
    }
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    const ip_event_got_ip_t* e = (const ip_event_got_ip_t*)data;
    snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
    s_connected = true;
    s_retries = 0;
    s_mode = NET_MODE_ONLINE;
    ESP_LOGI(TAG, "connected: http://%s", s_ip);

    // Credentials are stored only once they have actually worked. Saving them
    // when the form was submitted would persist a typo and lock the device out
    // of its own setup page on the next boot.
    if (s_trying) {
      s_trying = false;
      s_try_error[0] = '\0';
      creds_save(s_try_ssid, s_try_pass);
      // Leave the setup AP up briefly. The page asked over it and is waiting
      // for an answer; pulling the network out from under the reply means the
      // user sees a failed request after a successful join.
      if (s_ap_down_timer) esp_timer_start_once(s_ap_down_timer, 5000000);
    }
  }
}

// ------------------------------------------------------------------ api

static bool body_of(httpd_req_t* r, char* buf, size_t cap) {
  const size_t want = r->content_len < cap - 1 ? r->content_len : cap - 1;
  if (want == 0) {
    buf[0] = 0;
    return true;
  }
  // Read until the body is in, rather than once.
  //
  // httpd_req_recv returns what one TCP segment carried, which for the short
  // bodies this started with was always the whole thing. It is not a property
  // of the protocol, and a body that arrives split — which is any body large
  // enough to be worth sending — was silently truncated to its first segment
  // and then parsed as though it were complete.
  size_t got = 0;
  while (got < want) {
    const int n = httpd_req_recv(r, buf + got, want - got);
    if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;  // a pause, not an end
    if (n <= 0) return false;
    got += (size_t)n;
  }
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

int net_state_json(char* out, size_t cap) {
  char clock[8] = "--:--";
  if (s_time_valid) {
    const time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    snprintf(clock, sizeof(clock), "%02d:%02d", tm.tm_hour, tm.tm_min);
  }
  const int n = snprintf(
      out, cap,
      "{\"screen\":\"%s\",\"status\":\"%s\",\"brightness\":%u,"
      "\"timer_left\":%d,\"timer_running\":%s,\"fps\":%.1f,"
      "\"detents\":%ld,\"illegal\":%lu,\"ip\":\"%s\","
      "\"clock\":\"%s\",\"menu_index\":%d,"
      "\"set_label\":\"%s\",\"set_text\":\"%s\"}",
      s_status.screen, s_status.status, s_status.brightness, s_status.timer_left_s,
      s_status.timer_running ? "true" : "false", s_status.fps,
      (long)s_status.detents, (unsigned long)s_status.illegal, s_ip, clock,
      (int)s_status.menu_index, s_status.set_label, s_status.set_text);
  // snprintf returns what it *would* have written. Clamp, or the caller sends
  // a length that runs past the buffer it was given.
  if (n < 0) return 0;
  return n < (int)cap ? n : (int)cap - 1;
}

static esp_err_t get_state(httpd_req_t* r) {
  char buf[440];
  const int n = net_state_json(buf, sizeof(buf));
  httpd_resp_set_type(r, "application/json");
  return httpd_resp_send(r, buf, n);
}

static bool str_field(const char* body, const char* key, char* out, size_t cap) {
  const char* p = strstr(body, key);
  if (!p) return false;
  p = strchr(p + strlen(key), ':');
  if (!p) return false;
  while (*p && *p != '"') ++p;
  if (*p != '"') return false;
  ++p;
  size_t n = 0;
  while (*p && *p != '"' && n + 1 < cap) {
    // Only the escapes a password or an SSID can actually contain. Anything
    // else is passed through as written rather than guessed at.
    if (*p == '\\' && p[1]) {
      ++p;
      char c = *p;
      if (c == 'n') c = '\n';
      else if (c == 't') c = '\t';
      out[n++] = c;
      ++p;
      continue;
    }
    out[n++] = *p++;
  }
  if (*p != '"') return false;  // ran out of buffer or out of body
  out[n] = '\0';
  return true;
}

// The latest draw request, and a serial so the render loop can tell a new one
// from the same one still sitting here.
//
// Deliberately last-one-wins rather than a queue. A panel eight rows tall shows
// one thing; queueing messages behind each other would mean a host that sent
// five in a second had four of them shown to nobody, seconds late.
static net_draw_t s_draw;
static uint32_t s_draw_serial = 0;
static uint32_t s_draw_taken = 0;

bool net_take_draw(net_draw_t* out) {
  if (s_draw_serial == s_draw_taken) return false;
  s_draw_taken = s_draw_serial;
  *out = s_draw;
  return true;
}

// #RRGGBB or #RRGGBBAA, or a bare hex triple. Alpha is parsed and discarded:
// there is nothing to blend against on a panel that is its own background.
static uint32_t parse_colour(const char* body, uint32_t fallback) {
  char hex[12];
  if (!str_field(body, "\"color\"", hex, sizeof(hex))) return fallback;
  const char* p = (hex[0] == '#') ? hex + 1 : hex;
  uint32_t v = 0;
  int n = 0;
  for (; p[n] && n < 6; ++n) {
    const char c = p[n];
    uint32_t d;
    if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
    else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
    else return fallback;
    v = (v << 4) | d;
  }
  return n == 6 ? v : fallback;
}

static esp_err_t post_draw(httpd_req_t* r) {
  char body[320];
  if (!body_of(r, body, sizeof(body))) return httpd_resp_send_500(r);

  net_draw_t d = {0};
  d.priority = 50;
  d.ttl_s = 10.0f;
  d.bar = -1.0f;
  d.tint = 0xFF8A1F;

  str_field(body, "\"text\"", d.text, sizeof(d.text));
  str_field(body, "\"icon\"", d.icon, sizeof(d.icon));
  str_field(body, "\"source\"", d.source, sizeof(d.source));
  d.tint = parse_colour(body, d.tint);

  int v = 0;
  if (field(body, "\"priority\"", &v)) d.priority = (uint8_t)(v < 1 ? 1 : (v > 100 ? 100 : v));
  if (field(body, "\"ttl\"", &v)) d.ttl_s = (float)(v < 0 ? 0 : v);
  if (field(body, "\"bar\"", &v)) d.bar = (float)(v < 0 ? 0 : (v > 100 ? 100 : v)) / 100.0f;
  // A deadline, not a duration: a duration is already stale by however long
  // the request took to arrive, and this one may sit on screen for minutes.
  // field() returns an int, which runs out in 2038 — long after the rest of
  // this will have been rewritten, and the alternative is a second parser.
  if (field(body, "\"until\"", &v)) d.until_unix = (int64_t)(uint32_t)v;

  if (d.text[0] == 0 && d.icon[0] == 0 && d.until_unix == 0 && d.bar < 0.0f) {
    httpd_resp_set_status(r, "400 Bad Request");
    return httpd_resp_sendstr(r, "{\"error\":\"nothing to draw\"}");
  }

  s_draw = d;
  ++s_draw_serial;
  httpd_resp_set_type(r, "application/json");
  return httpd_resp_sendstr(r, "{\"ok\":true}");
}

static esp_err_t delete_draw(httpd_req_t* r) {
  // An empty payload with no ttl: the model drops it on the next frame.
  net_draw_t d = {0};
  d.priority = 100;   // outranks whatever is up, since the point is to clear it
  d.ttl_s = 0.001f;
  d.bar = -1.0f;
  s_draw = d;
  ++s_draw_serial;
  httpd_resp_set_type(r, "application/json");
  return httpd_resp_sendstr(r, "{\"cleared\":true}");
}

// The command vocabulary, independent of how it arrived.
//
// Extracted so that a write over Bluetooth and a POST over WiFi are the same
// input in the same words. Two transports parsing the same JSON slightly
// differently would be a bug nobody finds until the day one is unavailable and
// the other is all you have.
int net_apply_input_json(const char* body) {
  int v = 0, n = 0;
  if (field(body, "\"tap\"", &v)) { push(NET_CMD_TAP, (int16_t)v); ++n; }
  if (field(body, "\"hold\"", &v)) { push(NET_CMD_HOLD, (int16_t)v); ++n; }
  if (field(body, "\"release\"", &v)) { push(NET_CMD_RELEASE, (int16_t)v); ++n; }
  if (field(body, "\"swipe\"", &v)) { push(NET_CMD_SWIPE, (int16_t)v); ++n; }
  if (field(body, "\"turn\"", &v)) { push(NET_CMD_TURN, (int16_t)v); ++n; }
  if (field(body, "\"press\"", &v)) { push(NET_CMD_PRESS, 0); ++n; }
  if (field(body, "\"presshold\"", &v)) { push(NET_CMD_PRESS_HOLD, 0); ++n; }
  if (field(body, "\"status\"", &v)) { push(NET_CMD_STATUS, (int16_t)v); ++n; }
  if (field(body, "\"brightness\"", &v)) { push(NET_CMD_BRIGHTNESS, (int16_t)v); ++n; }
  return n;
}

static esp_err_t post_input(httpd_req_t* r) {
  char body[192];
  if (!body_of(r, body, sizeof(body))) return httpd_resp_send_500(r);
  net_apply_input_json(body);
  httpd_resp_set_type(r, "application/json");
  return httpd_resp_send(r, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

// Pulls a quoted string field out of the body. The same deliberately small
// parser as field(), for the same reason: one JSON object of three keys does
// not justify linking cJSON, and everything here is length-checked.

// Escapes a string into a JSON value. An SSID is 32 arbitrary octets and is
// chosen by someone else, so it can perfectly well contain a quote or a
// backslash — and emitting it raw would produce a broken document that the
// page fails to parse, for a network that is otherwise fine.
static void json_escape(const char* in, char* out, size_t cap) {
  size_t n = 0;
  for (const unsigned char* p = (const unsigned char*)in; *p && n + 7 < cap; ++p) {
    if (*p == '"' || *p == '\\') {
      out[n++] = '\\';
      out[n++] = (char)*p;
    } else if (*p < 0x20) {
      n += snprintf(out + n, cap - n, "\\u%04x", *p);
    } else {
      out[n++] = (char)*p;
    }
  }
  out[n] = '\0';
}

static esp_err_t get_wifi_scan(httpd_req_t* r) {
  // Blocking, on the server's own task. That is allowed here and not in the
  // event handler: this task exists to wait on things, and a scan is about two
  // seconds.
  //
  // Whether to drop the station first depends on what it is doing. A scan
  // returns nothing while a *join is in flight*, so an unconnected radio has
  // to be stood down first. An already-associated one must not be: the
  // request arrived over that association, and disconnecting kills the socket
  // the reply has to go back down. That is why this endpoint first returned
  // an empty body from the home network while working perfectly in setup.
  const bool stand_down = !s_connected;
  if (stand_down) {
    s_scanning = true;
    if (s_retry_timer) esp_timer_stop(s_retry_timer);
    esp_wifi_disconnect();
  }

  wifi_scan_config_t sc = {0};
  const esp_err_t err = esp_wifi_scan_start(&sc, true);
  uint16_t n = 0;
  if (err == ESP_OK) esp_wifi_scan_get_ap_num(&n);
  if (n > 20) n = 20;

  wifi_ap_record_t recs[20];
  if (n) esp_wifi_scan_get_ap_records(&n, recs);
  if (stand_down) s_scanning = false;

  char buf[1280];
  int at = snprintf(buf, sizeof(buf), "{\"networks\":[");
  for (uint16_t i = 0; i < n; ++i) {
    char ssid[96];
    json_escape((const char*)recs[i].ssid, ssid, sizeof(ssid));
    if (ssid[0] == '\0') continue;  // hidden: nothing for the user to pick
    const int need = snprintf(NULL, 0, "%s{\"ssid\":\"%s\",\"rssi\":%d,\"open\":%s}",
                              at > 13 ? "," : "", ssid, recs[i].rssi,
                              recs[i].authmode == WIFI_AUTH_OPEN ? "true" : "false");
    if (at + need + 4 > (int)sizeof(buf)) break;
    at += snprintf(buf + at, sizeof(buf) - at,
                   "%s{\"ssid\":\"%s\",\"rssi\":%d,\"open\":%s}",
                   at > 13 ? "," : "", ssid, recs[i].rssi,
                   recs[i].authmode == WIFI_AUTH_OPEN ? "true" : "false");
  }
  at += snprintf(buf + at, sizeof(buf) - at, "]}");

  // Only if we took it down. Calling connect on a live association would
  // tear down the very socket this reply is about to use.
  if (stand_down && !s_trying) esp_wifi_connect();

  httpd_resp_set_type(r, "application/json");
  return httpd_resp_send(r, buf, at);
}

static esp_err_t post_wifi_connect(httpd_req_t* r) {
  char body[256];
  if (!body_of(r, body, sizeof(body))) return httpd_resp_send_500(r);

  char ssid[33] = {0}, pass[65] = {0};
  if (!str_field(body, "\"ssid\"", ssid, sizeof(ssid)) || ssid[0] == '\0') {
    httpd_resp_set_status(r, "400 Bad Request");
    return httpd_resp_sendstr(r, "{\"error\":\"no ssid\"}");
  }
  str_field(body, "\"pass\"", pass, sizeof(pass));  // absent means an open network

  strlcpy(s_try_ssid, ssid, sizeof(s_try_ssid));
  strlcpy(s_try_pass, pass, sizeof(s_try_pass));
  s_try_error[0] = '\0';
  s_trying = true;

  wifi_config_t wc = {0};
  strlcpy((char*)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
  strlcpy((char*)wc.sta.password, pass, sizeof(wc.sta.password));
  esp_wifi_set_mode(WIFI_MODE_APSTA);
  esp_wifi_set_config(WIFI_IF_STA, &wc);
  if (s_retry_timer) esp_timer_stop(s_retry_timer);
  esp_wifi_disconnect();
  esp_wifi_connect();

  ESP_LOGI(TAG, "trying %s from the setup page", ssid);
  // 202: accepted, not done. The page polls /api/wifi/status for the outcome,
  // because a join takes seconds and holding the socket open for it would tie
  // up the one worker this server has.
  httpd_resp_set_status(r, "202 Accepted");
  httpd_resp_set_type(r, "application/json");
  return httpd_resp_sendstr(r, "{\"trying\":true}");
}

static esp_err_t get_wifi_status(httpd_req_t* r) {
  char err[112];
  json_escape(s_try_error, err, sizeof(err));
  char buf[256];
  const int n = snprintf(
      buf, sizeof(buf),
      "{\"mode\":\"%s\",\"trying\":%s,\"ip\":\"%s\",\"error\":\"%s\","
      "\"setup_ssid\":\"%s\"}",
      s_mode == NET_MODE_ONLINE ? "online"
                                : (s_mode == NET_MODE_JOINING ? "joining" : "setup"),
      s_trying ? "true" : "false", s_ip, err, s_ap_ssid);
  httpd_resp_set_type(r, "application/json");
  return httpd_resp_send(r, buf, n);
}

static esp_err_t post_wifi_forget(httpd_req_t* r) {
  creds_clear();
  ESP_LOGW(TAG, "credentials cleared; restarting into setup");
  httpd_resp_set_type(r, "application/json");
  httpd_resp_sendstr(r, "{\"forgotten\":true}");
  // Restart rather than unwind the radio in place. This is a rare, deliberate
  // act and a clean boot is the one path into setup that is certain to work.
  if (s_ap_down_timer) esp_timer_stop(s_ap_down_timer);
  esp_restart();
  return ESP_OK;
}

// Anything unrecognised becomes a redirect to the setup page.
//
// This is the half of the captive portal that the DNS responder cannot do on
// its own: the phone resolves its connectivity-check URL to us and then asks
// for a specific path, and it is this reply that tells it there is a portal
// here rather than an internet.
static esp_err_t redirect_to_portal(httpd_req_t* r, httpd_err_code_t e) {
  httpd_resp_set_status(r, "302 Found");
  httpd_resp_set_hdr(r, "Location", "http://192.168.4.1/");
  httpd_resp_send(r, NULL, 0);
  return ESP_OK;
}

static esp_err_t start_server(void) {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.max_open_sockets = 4;   // fewer sockets, less RAM; nothing needs more
  cfg.lru_purge_enable = true;  // a stuck client cannot lock the panel out
  cfg.max_uri_handlers = 12;
  // A scan builds its JSON on this stack, and so does the state handler.
  cfg.stack_size = 5120;
  // An image is about 875 KB and arrives in 4 KB pieces. The default five
  // second receive timeout ends the upload part way through on a slow link,
  // and a half-written slot is a wasted three minutes rather than a broken
  // device — but it is still three minutes.
  cfg.recv_wait_timeout = 20;
  cfg.send_wait_timeout = 20;
  ESP_RETURN_ON_ERROR(httpd_start(&s_server, &cfg), TAG, "httpd");

  const httpd_uri_t root = {"/", HTTP_GET, get_root, NULL};
  const httpd_uri_t state = {"/api/state", HTTP_GET, get_state, NULL};
  const httpd_uri_t input = {"/api/input", HTTP_POST, post_input, NULL};
  const httpd_uri_t scan = {"/api/wifi/scan", HTTP_GET, get_wifi_scan, NULL};
  const httpd_uri_t wstat = {"/api/wifi/status", HTTP_GET, get_wifi_status, NULL};
  const httpd_uri_t conn = {"/api/wifi/connect", HTTP_POST, post_wifi_connect, NULL};
  const httpd_uri_t forget = {"/api/wifi/forget", HTTP_POST, post_wifi_forget, NULL};
  const httpd_uri_t ota = {"/api/ota", HTTP_POST, ota_post, NULL};
  const httpd_uri_t draw = {"/api/display/draw", HTTP_POST, post_draw, NULL};
  const httpd_uri_t undraw = {"/api/display/draw", HTTP_DELETE, delete_draw, NULL};
  httpd_register_uri_handler(s_server, &root);
  httpd_register_uri_handler(s_server, &state);
  httpd_register_uri_handler(s_server, &input);
  httpd_register_uri_handler(s_server, &scan);
  httpd_register_uri_handler(s_server, &wstat);
  httpd_register_uri_handler(s_server, &conn);
  httpd_register_uri_handler(s_server, &forget);
  httpd_register_uri_handler(s_server, &ota);
  httpd_register_uri_handler(s_server, &draw);
  httpd_register_uri_handler(s_server, &undraw);
  // The captive-portal half that DNS cannot do. Harmless in station mode: a
  // wrong path on the home network redirects to a page that is not there,
  // which is no worse than the 404 it replaces.
  httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, redirect_to_portal);
  return ESP_OK;
}

// The server is started from the event that brings the address, on the event
// task, so app_main never waits for a network to come up.
static void on_got_ip(void* arg, esp_event_base_t base, int32_t id, void* data) {
  start_sntp();
  // Only in station mode: in setup the device *is* the network, its address is
  // fixed and printed on the panel, and there is nothing to discover.
  discovery_start(s_ap_ssid[0] ? s_ap_ssid : "pixelbar", net_ip);
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

  const esp_timer_create_args_t apargs = {.callback = stop_setup_ap,
                                          .name = "ap_down"};
  ESP_RETURN_ON_ERROR(esp_timer_create(&apargs, &s_ap_down_timer), TAG, "ap timer");

  // Bluetooth first, and unconditionally.
  //
  // It does not depend on credentials, on a router, or on anything being
  // discovered — which is exactly why it is worth having beside WiFi rather
  // than instead of it. A panel with no network at all is still a panel a Mac
  // beside it can drive.
  {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char name[24];
    snprintf(name, sizeof(name), "Pixelbar-%02X%02X", mac[4], mac[5]);
    if (ble_start(name) != ESP_OK) ESP_LOGW(TAG, "bluetooth unavailable");
  }

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

  // NVS is the only place credentials come from.
  //
  // They used to be compiled in from a secrets.h and seeded into NVS on first
  // boot. That is gone, and not only because the setup page replaced it. A
  // password written into a header does not stay in the header: it lands in
  // the object file, the static library, the .elf and the .bin, so an
  // ordinary build tree quietly becomes five more copies of a secret that
  // nobody thinks of as holding one. Nothing in these sources has ever seen a
  // credential now, which is the only way to be sure no build artifact has
  // either.
  char ssid[33] = {0}, pass[65] = {0};
  creds_load(ssid, sizeof(ssid), pass, sizeof(pass));

  if (ssid[0] == '\0') {
    // Nothing stored. Straight to setup — and the
    // server comes up here rather than on IP_EVENT_STA_GOT_IP, because in
    // setup mode there will not be one.
    ESP_LOGI(TAG, "no credentials: opening setup");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_max_tx_power(kTxPowerQuarterDbm);
    start_setup_ap();
    if (start_server() == ESP_OK) ESP_LOGI(TAG, "setup page on http://192.168.4.1");
    return ESP_OK;
  }

  s_mode = NET_MODE_JOINING;
  wifi_config_t wc = {0};
  strlcpy((char*)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
  strlcpy((char*)wc.sta.password, pass, sizeof(wc.sta.password));
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

  // The stored SSID, which is broadcast in the clear anyway. The password
  // is never logged, not even its length.
  ESP_LOGI(TAG, "joining %s", ssid);
  xTaskCreate(scan_task, "wifi_scan", 3072, NULL, 3, NULL);
  return ESP_OK;
}

float net_ota_progress(void) { return ota_progress(); }
uint32_t net_passkey(void) { return ble_passkey(); }
void net_mark_healthy(void) { ota_mark_healthy(); }

bool net_connected(void) { return s_connected; }
const char* net_ip(void) { return s_ip; }
void net_publish(const net_status_t* s) {
  s_status = *s;
  // Notified at about 4 Hz rather than the 100 Hz this is called at. A BLE
  // connection event is a scheduled slot on a radio shared with WiFi, and
  // filling every one of them with a status nobody asked for is how you make
  // both transports worse.
  static int64_t last_us = 0;
  const int64_t now = esp_timer_get_time();
  if (now - last_us >= 250000) {
    last_us = now;
    ble_publish();
  }
}

bool net_take_cmd(net_cmd_t* out) {
  if (!s_cmds) return false;
  return xQueueReceive(s_cmds, out, 0) == pdTRUE;
}
