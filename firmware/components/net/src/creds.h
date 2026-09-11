// The stored WiFi credentials. Private to the net component.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// False when nothing is stored, which is what puts the device into setup mode.
bool creds_load(char* ssid, size_t ssid_cap, char* pass, size_t pass_cap);
// An empty or null SSID is refused: it would store a blob that reads back as
// no credentials at all, which is a confusing way to spell creds_clear().
bool creds_save(const char* ssid, const char* pass);
bool creds_clear(void);

// Whether the build's credentials have ever been seeded into NVS. Seeding is
// a once-per-device event, not a once-per-empty-NVS one: see creds.c.
bool creds_seeded(void);
bool creds_mark_seeded(void);

#ifdef __cplusplus
}
#endif
