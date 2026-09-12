// The Bluetooth transport. Private to the net component.
#pragma once
#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Brings up NimBLE and starts advertising. Returns as soon as the stack is
// started; the radio is ready a moment later, on its sync callback.
esp_err_t ble_start(const char* name);

bool ble_connected(void);

// Notifies the state characteristic, if anything is connected and listening.
// Cheap enough to call on a timer, not cheap enough to call every frame.
void ble_publish(void);

#ifdef __cplusplus
}
#endif
