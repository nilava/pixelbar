// A DNS server that gives the same answer to everything.
//
// This is what makes the setup page open by itself. A phone that joins a
// network immediately fetches a known URL to find out whether it is really on
// the internet — captive.apple.com for iOS, connectivitycheck.gstatic.com for
// Android. Resolving those to us, and answering the fetch with a redirect, is
// what the phone recognises as a captive portal, and it pops the page up
// without anyone typing an address.
//
// So this answers every A query with our own address, whatever was asked. That
// is a lie, and it is confined to a setup network that exists for ninety
// seconds and has no route to anywhere: there is nothing else on it to reach.
// It runs only in AP mode and is stopped the moment credentials are stored.
//
// Sixty lines of socket code rather than a DNS library, because the only query
// worth parsing is one we have already decided the answer to.
#include "captive_dns.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char* TAG = "cdns";

static TaskHandle_t s_task = NULL;
static int s_sock = -1;
static uint32_t s_answer = 0;  // network byte order

// The 12-byte header, then the question, then our answer appended.
typedef struct __attribute__((packed)) {
  uint16_t id, flags, qdcount, ancount, nscount, arcount;
} dns_header_t;

// A pointer to the question's name at offset 12, rather than repeating it.
typedef struct __attribute__((packed)) {
  uint16_t name_ptr;  // 0xC00C
  uint16_t type, cls;
  uint32_t ttl;
  uint16_t rdlength;
  uint32_t rdata;
} dns_answer_t;

static void dns_task(void* arg) {
  uint8_t buf[512];
  while (1) {
    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);
    const int n = recvfrom(s_sock, buf, sizeof(buf), 0, (struct sockaddr*)&from,
                           &from_len);
    if (n < 0) break;  // the socket was closed under us: that is the stop signal
    if (n < (int)sizeof(dns_header_t)) continue;

    dns_header_t* h = (dns_header_t*)buf;
    if (ntohs(h->qdcount) != 1) continue;

    // Walk the QNAME's length-prefixed labels to find where the question ends.
    int p = sizeof(dns_header_t);
    while (p < n && buf[p] != 0) {
      const int label = buf[p];
      if (label & 0xC0) break;  // compression in a query: not worth handling
      p += label + 1;
    }
    if (p >= n || buf[p] != 0) continue;
    p += 1 + 4;  // the root label, then QTYPE and QCLASS
    if (p > n) continue;
    if (p + (int)sizeof(dns_answer_t) > (int)sizeof(buf)) continue;

    h->flags = htons(0x8180);  // response, recursion available, no error
    h->ancount = htons(1);
    h->nscount = 0;
    h->arcount = 0;

    dns_answer_t a = {
        .name_ptr = htons(0xC00C),  // the name is at offset 12, in the question
        .type = htons(1),           // A
        .cls = htons(1),            // IN
        // Short, so that nothing is still pointing here once the device has
        // left setup mode and joined the real network.
        .ttl = htonl(10),
        .rdlength = htons(4),
        .rdata = s_answer,
    };
    memcpy(buf + p, &a, sizeof(a));
    sendto(s_sock, buf, p + sizeof(a), 0, (struct sockaddr*)&from, from_len);
  }
  s_task = NULL;
  vTaskDelete(NULL);
}

esp_err_t captive_dns_start(uint32_t answer_ip_be) {
  if (s_task) return ESP_OK;
  s_answer = answer_ip_be;

  s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s_sock < 0) return ESP_FAIL;

  struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_port = htons(53),
      .sin_addr = {.s_addr = htonl(INADDR_ANY)},
  };
  if (bind(s_sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
    close(s_sock);
    s_sock = -1;
    return ESP_FAIL;
  }
  if (xTaskCreate(dns_task, "captive_dns", 3072, NULL, 4, &s_task) != pdPASS) {
    close(s_sock);
    s_sock = -1;
    return ESP_ERR_NO_MEM;
  }
  ESP_LOGI(TAG, "captive DNS up");
  return ESP_OK;
}

void captive_dns_stop(void) {
  if (s_sock >= 0) {
    // Closing the socket is what breaks recvfrom, which is what ends the task.
    // Deleting the task from here instead would leak the socket and could free
    // its stack while lwIP was still inside it.
    const int s = s_sock;
    s_sock = -1;
    shutdown(s, SHUT_RDWR);
    close(s);
  }
}
