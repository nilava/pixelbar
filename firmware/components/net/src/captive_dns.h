// The captive-portal DNS responder. Private to the net component.
#pragma once
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Answers every A query with this address, which must already be in network
// byte order. Idempotent: calling it twice is not an error.
esp_err_t captive_dns_start(uint32_t answer_ip_be);
void captive_dns_stop(void);

#ifdef __cplusplus
}
#endif
