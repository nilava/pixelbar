// The panel as a media remote, over Bluetooth HID.
//
// Separate from the rest of `ble.c` because it answers a different question.
// Everything else there is the panel talking to *our* software; this is the
// panel talking to an operating system that has never heard of Pixelbar — no
// helper, no token, no app, on any Mac, iPhone, Windows box or Linux machine
// that can pair a Bluetooth remote.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Which key, as bits in the one-byte Consumer Control report. Deliberately a
// bitmap rather than an enum: a report says which keys are down *now*, so
// releasing is sending zero rather than sending a different message.
typedef enum {
  HID_KEY_PLAY_PAUSE = 1u << 0,
  HID_KEY_NEXT = 1u << 1,
  HID_KEY_PREV = 1u << 2,
  HID_KEY_VOL_UP = 1u << 3,
  HID_KEY_VOL_DOWN = 1u << 4,
  HID_KEY_MUTE = 1u << 5,
} hid_key_t;

// Adds the HID, battery and device-information services to the GATT table.
// Call before ble_gatts_count_cfg, alongside the panel's own service.
int hid_register(void);

// Press and release, which is what every one of these keys means: there is no
// held state worth carrying, and a host that never sees the release repeats
// the key forever.
void hid_tap(hid_key_t key);

// Whether a host has subscribed to the report characteristic — the only
// evidence anything is listening. Sending to nobody is not an error but it is
// worth being able to say so on the panel.
bool hid_connected(void);

// Called from the BLE event handler.
void hid_on_subscribe(uint16_t attr_handle, bool on);
void hid_on_disconnect(void);
void hid_set_conn(uint16_t conn);

#ifdef __cplusplus
}
#endif
