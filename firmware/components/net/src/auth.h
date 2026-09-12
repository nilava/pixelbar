// The API token and the pairing window. Private to the net component.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Loads the token from NVS, or makes one on first boot.
void auth_init(void);
const char* auth_token(void);

// Opens a pairing window and returns the six-digit code to display.
uint32_t auth_begin_pairing(void);
// The code currently on the panel, or 0 if no window is open or it expired.
uint32_t auth_pairing_code(void);
// Exchanges a correct code for the token, once.
bool auth_redeem(uint32_t code, const char** token_out);

// Whether a presented token is the right one.
bool auth_ok(const char* presented);

#ifdef __cplusplus
}
#endif
