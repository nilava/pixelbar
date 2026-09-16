#include "hid.h"

#include <string.h>

#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "os/os_mbuf.h"

static const char* TAG = "hid";

// The report descriptor: one byte, six Consumer Control keys.
//
// Consumer Control rather than Keyboard, and that is the whole reason this
// works with no software. A keyboard would need the host to decide what a
// keystroke means; volume, transport and mute are usages the operating system
// already acts on itself, at the same layer as the keys on a laptop's top row.
//
// Six usages and two bits of constant padding, because a report has to be a
// whole number of bytes and a host is entitled to reject one that is not.
static const uint8_t kReportMap[] = {
    0x05, 0x0C,  // Usage Page (Consumer)
    0x09, 0x01,  //   Usage (Consumer Control)
    0xA1, 0x01,  //   Collection (Application)
    0x85, 0x01,  //     Report ID (1)
    0x15, 0x00,  //     Logical Minimum (0)
    0x25, 0x01,  //     Logical Maximum (1)
    0x75, 0x01,  //     Report Size (1 bit)
    0x95, 0x06,  //     Report Count (6)
    0x09, 0xCD,  //     Usage (Play/Pause)        bit 0
    0x09, 0xB5,  //     Usage (Scan Next Track)   bit 1
    0x09, 0xB6,  //     Usage (Scan Prev Track)   bit 2
    0x09, 0xE9,  //     Usage (Volume Up)         bit 3
    0x09, 0xEA,  //     Usage (Volume Down)       bit 4
    0x09, 0xE2,  //     Usage (Mute)              bit 5
    0x81, 0x02,  //     Input (Data, Variable, Absolute)
    0x75, 0x01,  //     Report Size (1 bit)
    0x95, 0x02,  //     Report Count (2)
    0x81, 0x03,  //     Input (Constant) — padding to a whole byte
    0xC0,        //   End Collection
};

// bcdHID 1.11, country 0, flags: NormallyConnectable | RemoteWake.
static const uint8_t kHidInfo[4] = {0x11, 0x01, 0x00, 0x03};

// PnP ID. The vendor is Espressif's own USB ID, because that is whose silicon
// this is; claiming somebody else's would be a lie told to every host that
// asks. Some stacks refuse a HID device without this characteristic.
static const uint8_t kPnpId[7] = {
    0x02,        // vendor ID source: USB Implementer's Forum
    0x3A, 0x30,  // vendor: 0x303A, Espressif
    0x01, 0x00,  // product
    0x00, 0x01,  // version 1.0.0
};

static uint16_t s_report_handle;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static bool s_subscribed;

static int on_report_map(uint16_t conn, uint16_t attr,
                         struct ble_gatt_access_ctxt* ctxt, void* arg) {
  return os_mbuf_append(ctxt->om, kReportMap, sizeof(kReportMap)) == 0
             ? 0
             : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int on_hid_info(uint16_t conn, uint16_t attr,
                       struct ble_gatt_access_ctxt* ctxt, void* arg) {
  return os_mbuf_append(ctxt->om, kHidInfo, sizeof(kHidInfo)) == 0
             ? 0
             : BLE_ATT_ERR_INSUFFICIENT_RES;
}

// Reading the report returns the resting state: nothing held.
static int on_report(uint16_t conn, uint16_t attr,
                     struct ble_gatt_access_ctxt* ctxt, void* arg) {
  const uint8_t none = 0;
  return os_mbuf_append(ctxt->om, &none, 1) == 0 ? 0
                                                 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

// The report reference descriptor: report ID 1, type 1 (Input). Without it a
// host cannot tell which report map entry this characteristic carries, and
// most simply ignore the device.
static int on_report_ref(uint16_t conn, uint16_t attr,
                         struct ble_gatt_access_ctxt* ctxt, void* arg) {
  const uint8_t ref[2] = {0x01, 0x01};
  return os_mbuf_append(ctxt->om, ref, sizeof(ref)) == 0
             ? 0
             : BLE_ATT_ERR_INSUFFICIENT_RES;
}

// Suspend and exit-suspend. Nothing here sleeps on the host's say-so — the
// panel has its own idea of when to dim — but the characteristic is mandatory
// and a host that cannot write it may refuse the device.
static int on_control_point(uint16_t conn, uint16_t attr,
                            struct ble_gatt_access_ctxt* ctxt, void* arg) {
  return 0;
}

// Report protocol, always. Boot protocol is for keyboards and mice in a BIOS,
// which a media remote never is.
static int on_protocol_mode(uint16_t conn, uint16_t attr,
                            struct ble_gatt_access_ctxt* ctxt, void* arg) {
  if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
    const uint8_t mode = 0x01;
    return os_mbuf_append(ctxt->om, &mode, 1) == 0
               ? 0
               : BLE_ATT_ERR_INSUFFICIENT_RES;
  }
  return 0;
}

// Mains powered, and saying so beats leaving the host to guess. HID over GATT
// expects a battery service; a panel plugged into a wall reports full.
static int on_battery(uint16_t conn, uint16_t attr,
                      struct ble_gatt_access_ctxt* ctxt, void* arg) {
  const uint8_t full = 100;
  return os_mbuf_append(ctxt->om, &full, 1) == 0 ? 0
                                                 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int on_pnp(uint16_t conn, uint16_t attr,
                  struct ble_gatt_access_ctxt* ctxt, void* arg) {
  return os_mbuf_append(ctxt->om, kPnpId, sizeof(kPnpId)) == 0
             ? 0
             : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static const ble_uuid16_t kUuidHidSvc = BLE_UUID16_INIT(0x1812);
static const ble_uuid16_t kUuidHidInfo = BLE_UUID16_INIT(0x2A4A);
static const ble_uuid16_t kUuidReportMap = BLE_UUID16_INIT(0x2A4B);
static const ble_uuid16_t kUuidControlPoint = BLE_UUID16_INIT(0x2A4C);
static const ble_uuid16_t kUuidReport = BLE_UUID16_INIT(0x2A4D);
static const ble_uuid16_t kUuidProtocolMode = BLE_UUID16_INIT(0x2A4E);
static const ble_uuid16_t kUuidReportRef = BLE_UUID16_INIT(0x2908);
static const ble_uuid16_t kUuidBatterySvc = BLE_UUID16_INIT(0x180F);
static const ble_uuid16_t kUuidBatteryLevel = BLE_UUID16_INIT(0x2A19);
static const ble_uuid16_t kUuidDeviceInfoSvc = BLE_UUID16_INIT(0x180A);
static const ble_uuid16_t kUuidPnpId = BLE_UUID16_INIT(0x2A50);

static const struct ble_gatt_svc_def kHidServices[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &kUuidHidSvc.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &kUuidHidInfo.u,
                .access_cb = on_hid_info,
                .flags = BLE_GATT_CHR_F_READ,
            },
            {
                .uuid = &kUuidReportMap.u,
                .access_cb = on_report_map,
                // Encrypted, as the profile requires. The bond that the panel's
                // own service already needs satisfies this too, so pairing once
                // covers both.
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC,
            },
            {
                .uuid = &kUuidControlPoint.u,
                .access_cb = on_control_point,
                .flags = BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &kUuidProtocolMode.u,
                .access_cb = on_protocol_mode,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &kUuidReport.u,
                .access_cb = on_report,
                .val_handle = &s_report_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY |
                         BLE_GATT_CHR_F_READ_ENC,
                .descriptors = (struct ble_gatt_dsc_def[]){
                    {
                        .uuid = &kUuidReportRef.u,
                        .att_flags = BLE_ATT_F_READ,
                        .access_cb = on_report_ref,
                    },
                    {0},
                },
            },
            {0},
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &kUuidBatterySvc.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &kUuidBatteryLevel.u,
                .access_cb = on_battery,
                .flags = BLE_GATT_CHR_F_READ,
            },
            {0},
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &kUuidDeviceInfoSvc.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &kUuidPnpId.u,
                .access_cb = on_pnp,
                .flags = BLE_GATT_CHR_F_READ,
            },
            {0},
        },
    },
    {0},
};

int hid_register(void) {
  int rc = ble_gatts_count_cfg(kHidServices);
  if (rc != 0) return rc;
  return ble_gatts_add_svcs(kHidServices);
}

bool hid_connected(void) { return s_subscribed; }

void hid_on_subscribe(uint16_t attr_handle, bool on) {
  if (attr_handle != s_report_handle) return;
  s_subscribed = on;
  ESP_LOGI(TAG, "media keys %s", on ? "subscribed" : "unsubscribed");
}

void hid_on_disconnect(void) { s_subscribed = false; }

void hid_set_conn(uint16_t conn) { s_conn = conn; }

static void send(uint8_t bits) {
  if (s_conn == BLE_HS_CONN_HANDLE_NONE || s_report_handle == 0) return;
  if (!s_subscribed) return;
  struct os_mbuf* om = ble_hs_mbuf_from_flat(&bits, 1);
  if (!om) return;
  ble_gatts_notify_custom(s_conn, s_report_handle, om);
}

void hid_tap(hid_key_t key) {
  // Down then straight up. A consumer-control report says which keys are held,
  // so a press with no release is a key held forever: the host repeats volume
  // until it reaches the end, and play/pause toggles until somebody unplugs
  // something. The release is not politeness, it is the other half of the
  // message.
  send((uint8_t)key);
  send(0);
}
