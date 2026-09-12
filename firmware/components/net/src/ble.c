// The same device, over Bluetooth.
//
// WiFi is the better pipe when it exists: it carries a firmware image, serves
// a web page, and reaches the device from anywhere in the house. But it only
// exists once somebody has provisioned it, it goes away when the router does,
// and finding the device on it took a broadcast beacon and a subnet sweep.
// Bluetooth has none of those problems and none of those abilities. So: both,
// and the same vocabulary on each.
//
// NimBLE rather than Bluedroid. Bluedroid is the fuller stack and roughly
// three times the flash; there is one app slot of 1.66 MB shared with WiFi,
// lwIP, an HTTP server and OTA, and this has to fit beside all of it.
//
// The characteristics are deliberately the same JSON the HTTP API speaks. Two
// transports that parsed the same commands slightly differently would be a bug
// nobody finds until the day one is unavailable and the other is all there is.
#include "ble.h"

#include <string.h>

#include "esp_log.h"
#include "esp_nimble_hci.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char* TAG = "ble";

// Declared in net.c: one parser and one state builder, shared with HTTP.
int net_apply_input_json(const char* body);
int net_state_json(char* out, size_t cap);

// 6f9a0001-… — a random base, with the last 16 bits naming each attribute.
// Nothing here is standard, so nothing here pretends to be: a made-up service
// with an assigned-numbers UUID would collide with whatever really owns it.
static const ble_uuid128_t kSvcUuid =
    BLE_UUID128_INIT(0x9a, 0x6f, 0x21, 0x0c, 0x7d, 0x4e, 0x11, 0xa3,
                     0x4b, 0x5e, 0x01, 0x00, 0x9a, 0x6f, 0x00, 0x00);
static const ble_uuid128_t kCmdUuid =
    BLE_UUID128_INIT(0x9a, 0x6f, 0x21, 0x0c, 0x7d, 0x4e, 0x11, 0xa3,
                     0x4b, 0x5e, 0x02, 0x00, 0x9a, 0x6f, 0x00, 0x00);
static const ble_uuid128_t kStateUuid =
    BLE_UUID128_INIT(0x9a, 0x6f, 0x21, 0x0c, 0x7d, 0x4e, 0x11, 0xa3,
                     0x4b, 0x5e, 0x03, 0x00, 0x9a, 0x6f, 0x00, 0x00);

static uint8_t s_addr_type;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_state_handle;
static char s_name[24] = "Pixelbar";

bool ble_connected(void) { return s_conn != BLE_HS_CONN_HANDLE_NONE; }

static void advertise(void);

static int on_cmd(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt* ctxt,
                  void* arg) {
  char body[192];
  const uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
  if (len == 0 || len >= sizeof(body)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
  uint16_t got = 0;
  if (ble_hs_mbuf_to_flat(ctxt->om, body, sizeof(body) - 1, &got) != 0)
    return BLE_ATT_ERR_UNLIKELY;
  body[got] = '\0';
  net_apply_input_json(body);
  return 0;
}

static int on_state(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt* ctxt,
                    void* arg) {
  char buf[440];
  const int n = net_state_json(buf, sizeof(buf));
  // A default MTU carries 20 bytes and this is about 200, so a central that
  // has not negotiated a larger one gets a truncated document. That is the
  // central's decision to make, and every one worth talking to asks for more.
  return os_mbuf_append(ctxt->om, buf, n) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static const struct ble_gatt_svc_def kServices[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &kSvcUuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                // Write without response: a status push is fire-and-forget and
                // waiting for an acknowledgement would double the latency of
                // the one thing this is for.
                .uuid = &kCmdUuid.u,
                .access_cb = on_cmd,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &kStateUuid.u,
                .access_cb = on_state,
                .val_handle = &s_state_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            {0},
        },
    },
    {0},
};

static int on_gap(struct ble_gap_event* ev, void* arg) {
  switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
      if (ev->connect.status == 0) {
        s_conn = ev->connect.conn_handle;
        ESP_LOGI(TAG, "connected");
      } else {
        advertise();
      }
      return 0;
    case BLE_GAP_EVENT_DISCONNECT:
      s_conn = BLE_HS_CONN_HANDLE_NONE;
      ESP_LOGI(TAG, "disconnected (%d)", ev->disconnect.reason);
      advertise();
      return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
      advertise();
      return 0;
    default:
      return 0;
  }
}

static void advertise(void) {
  // The name in the advertisement, the service UUID in the scan response.
  //
  // They do not both fit. An advertisement is 31 bytes: three for the flags,
  // fifteen for "Pixelbar-F8A4", and eighteen for a 128-bit UUID is
  // thirty-six. The first version of this tried both and fell back to dropping
  // the UUID — which worked, in the sense that it advertised, and left every
  // central filtering by service UUID unable to see the device at all. That is
  // how CoreBluetooth scans, so the panel was invisible to the thing it exists
  // to talk to.
  //
  // The scan response is the second packet, sent when an active scanner asks
  // for more. CoreBluetooth scans actively by default, so the filter matches;
  // a passive scanner still sees the name.
  struct ble_hs_adv_fields fields = {0};
  fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
  fields.name = (uint8_t*)s_name;
  fields.name_len = strlen(s_name);
  fields.name_is_complete = 1;
  if (ble_gap_adv_set_fields(&fields) != 0) ESP_LOGW(TAG, "advertisement rejected");

  struct ble_hs_adv_fields rsp = {0};
  rsp.uuids128 = (ble_uuid128_t*)&kSvcUuid;
  rsp.num_uuids128 = 1;
  rsp.uuids128_is_complete = 1;
  if (ble_gap_adv_rsp_set_fields(&rsp) != 0) ESP_LOGW(TAG, "scan response rejected");

  struct ble_gap_adv_params adv = {0};
  adv.conn_mode = BLE_GAP_CONN_MODE_UND;
  adv.disc_mode = BLE_GAP_DISC_MODE_GEN;
  // Slowly. This is the number that matters for this device.
  //
  // NimBLE's default is 30-60 ms, and on a single-core chip with one radio
  // shared with WiFi that took the frame rate from 100 fps to about 65 —
  // measured, not guessed. A third of the animation budget is a lot to pay for
  // being found quickly by something that scans for several seconds anyway.
  //
  // Units are 0.625 ms, so this is 500-760 ms: about fifteen times less radio
  // time, and still well inside the window any scanner uses.
  adv.itvl_min = 800;
  adv.itvl_max = 1216;
  ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &adv, on_gap, NULL);
}

static void on_sync(void) {
  ble_hs_util_ensure_addr(0);
  ble_hs_id_infer_auto(0, &s_addr_type);
  uint8_t addr[6] = {0};
  ble_hs_id_copy_addr(s_addr_type, addr, NULL);
  ESP_LOGI(TAG, "advertising as \"%s\" (%02x:%02x:%02x:%02x:%02x:%02x)", s_name,
           addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
  advertise();
}

static void on_reset(int reason) { ESP_LOGW(TAG, "stack reset: %d", reason); }

static void host_task(void* arg) {
  nimble_port_run();
  nimble_port_freertos_deinit();
}

esp_err_t ble_start(const char* name) {
  if (name && name[0]) snprintf(s_name, sizeof(s_name), "%s", name);

  esp_err_t err = nimble_port_init();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "nimble init: %s", esp_err_to_name(err));
    return err;
  }

  ble_hs_cfg.sync_cb = on_sync;
  ble_hs_cfg.reset_cb = on_reset;

  ble_svc_gap_init();
  ble_svc_gatt_init();
  if (ble_gatts_count_cfg(kServices) != 0 || ble_gatts_add_svcs(kServices) != 0) {
    ESP_LOGE(TAG, "could not register the service");
    return ESP_FAIL;
  }
  ble_svc_gap_device_name_set(s_name);

  nimble_port_freertos_init(host_task);
  return ESP_OK;
}

void ble_publish(void) {
  if (s_conn == BLE_HS_CONN_HANDLE_NONE || s_state_handle == 0) return;
  char buf[440];
  const int n = net_state_json(buf, sizeof(buf));
  struct os_mbuf* om = ble_hs_mbuf_from_flat(buf, n);
  if (!om) return;
  // Failure here is ordinary: the central may not have subscribed, or may have
  // gone away between the check above and now.
  ble_gatts_notify_custom(s_conn, s_state_handle, om);
}
