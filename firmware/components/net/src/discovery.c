// Answering "is there a Pixelbar on this network?"
//
// The alternative was mDNS, and it was rejected for a stated reason: mDNS is a
// managed component in IDF 5.x (`espressif/mdns`), and pulling it in breaks the
// README's promise that this project builds offline with nothing to download.
// A correct mDNS responder is also a great deal more than this — PTR, SRV, TXT
// and A records, name conflict resolution, and politeness towards every other
// responder on the segment — to answer one question asked by one helper.
//
// So: a datagram. A probe arrives on a fixed port, a reply goes back to
// whoever sent it. Fifty lines against a dependency, and the same socket
// pattern the captive portal already uses.
//
// This does mean the device is discoverable by anything on the LAN that knows
// the magic word, which is true of its HTTP server already.
#include "discovery.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char* TAG = "disco";

#define DISCOVERY_PORT 51737
// Deliberately not JSON, and deliberately not a single byte. A word this
// specific will not be sent by anything else, so a stray packet on this port
// is ignored rather than answered.
#define DISCOVERY_PROBE "PIXELBAR?"

static TaskHandle_t s_task = NULL;
static int s_sock = -1;
static char s_name[32] = "pixelbar";
static const char* (*s_ip_fn)(void) = NULL;

static void disco_task(void* arg) {
  char buf[64];
  while (1) {
    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);
    const int n = recvfrom(s_sock, buf, sizeof(buf) - 1, 0,
                           (struct sockaddr*)&from, &from_len);
    if (n < 0) break;  // the socket was closed under us: that is the stop signal
    buf[n] = '\0';

    if (strncmp(buf, DISCOVERY_PROBE, strlen(DISCOVERY_PROBE)) != 0) continue;

    char reply[128];
    const char* ip = s_ip_fn ? s_ip_fn() : "";
    const int len = snprintf(reply, sizeof(reply),
                             "{\"device\":\"pixelbar\",\"name\":\"%s\",\"ip\":\"%s\"}",
                             s_name, ip);
    // Unicast back to the asker rather than broadcast: everyone else on the
    // segment asked nothing and does not need the answer.
    sendto(s_sock, reply, len > (int)sizeof(reply) ? (int)sizeof(reply) : len, 0,
           (struct sockaddr*)&from, from_len);
  }
  s_task = NULL;
  vTaskDelete(NULL);
}

esp_err_t discovery_start(const char* name, const char* (*ip_fn)(void)) {
  if (s_task) return ESP_OK;
  if (name && name[0]) snprintf(s_name, sizeof(s_name), "%s", name);
  s_ip_fn = ip_fn;

  s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s_sock < 0) return ESP_FAIL;

  // Without this the stack drops datagrams sent to the broadcast address,
  // which is the only address a helper that does not know where we are can
  // possibly use.
  int yes = 1;
  setsockopt(s_sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));

  struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_port = htons(DISCOVERY_PORT),
      .sin_addr = {.s_addr = htonl(INADDR_ANY)},
  };
  if (bind(s_sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
    close(s_sock);
    s_sock = -1;
    return ESP_FAIL;
  }
  if (xTaskCreate(disco_task, "discovery", 3072, NULL, 4, &s_task) != pdPASS) {
    close(s_sock);
    s_sock = -1;
    return ESP_ERR_NO_MEM;
  }
  ESP_LOGI(TAG, "discoverable as \"%s\" on UDP %d", s_name, DISCOVERY_PORT);
  return ESP_OK;
}

void discovery_stop(void) {
  if (s_sock >= 0) {
    // Closing the socket is what breaks recvfrom, which is what ends the task.
    const int s = s_sock;
    s_sock = -1;
    shutdown(s, SHUT_RDWR);
    close(s);
  }
}
