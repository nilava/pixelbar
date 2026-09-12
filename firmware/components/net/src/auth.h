// The API token and the pairing window. Private to the net component.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Loads the token from NVS, or makes one on first boot.
void auth_init(void);

// Opens a pairing window and returns the six-digit code to display.
uint32_t auth_begin_pairing(void);
// The code currently on the panel, or 0 if no window is open or it expired.
uint32_t auth_pairing_code(void);
// Exchanges a correct code for a token of that client's own, once. `name` is
// what the host calls itself, for the paired list; the token is returned but
// never stored anywhere else and never logged.
bool auth_redeem(uint32_t code, const char* name, const char** token_out);

// Whether there is room for another client.
bool auth_full(void);

// The paired clients, for listing on the panel and on the page. Never returns
// a token: a list of who has access must not be a way to acquire it.
int auth_client_count(void);
int auth_client_max(void);
const char* auth_client_name(int i);
uint32_t auth_client_issued(int i);
bool auth_revoke(int i);
void auth_revoke_all(void);

// Whether a presented token is the right one.
bool auth_ok(const char* presented);

#ifdef __cplusplus
}
#endif
