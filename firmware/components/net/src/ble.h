// The Bluetooth transport. Private to the net component.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Brings up NimBLE and starts advertising. Returns as soon as the stack is
// started; the radio is ready a moment later, on its sync callback.
esp_err_t ble_start(const char* name);

bool ble_connected(void);

// The six-digit pairing code currently on the panel, or 0 when not pairing.
// Displaying it is what makes the bond mean something: a code that exists only
// on a panel in front of you proves the thing you paired with is the thing you
// are looking at.
uint32_t ble_passkey(void);

// Notifies the state characteristic, if anything is connected and listening.
// Cheap enough to call on a timer, not cheap enough to call every frame.
void ble_publish(void);

#ifdef __cplusplus
}
#endif
