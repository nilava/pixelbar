// The UDP discovery beacon. Private to the net component.
#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Starts answering discovery probes. `ip_fn` is called when a probe arrives
// rather than captured once, because the address is not known at the moment
// this is set up and can change under a DHCP lease. Idempotent.
esp_err_t discovery_start(const char* name, const char* (*ip_fn)(void));
void discovery_stop(void);

#ifdef __cplusplus
}
#endif
