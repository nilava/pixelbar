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
#include "esp_random.h"
#include "esp_nimble_hci.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "store/config/ble_store_config.h"

static const char* TAG = "ble";

// Declared here because ESP-IDF does not declare it anywhere.
//
// store/config/ble_store_config.h is included above and does not carry this
// prototype; upstream calls it from a Mynewt sysinit stage that the ESP port
// does not run, and IDF's own NimBLE examples forward-declare it exactly like
// this. Without the declaration the call compiles to an implicit int-returning
// function under -Werror, and without the call there is no bond store at all.
extern void ble_store_config_init(void);

// Declared in net.c: one parser and one state builder, shared with HTTP.
int net_apply_input_json(const char* body);
int net_state_json(char* out, size_t cap);
void net_request_scan(void);
const char* net_scan_json(void);
void net_provision(const char* ssid, const char* pass);
int net_link_json(char* out, size_t cap);
bool auth_issue(const char* name, const char** token_out);
const char* net_ip(void);

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

// Onboarding. All four require an authenticated link, so none of them is
// reachable without the passkey — which is what makes handing over an API
// token across this link reasonable.
static const ble_uuid128_t kScanUuid =
    BLE_UUID128_INIT(0x9a, 0x6f, 0x21, 0x0c, 0x7d, 0x4e, 0x11, 0xa3,
                     0x4b, 0x5e, 0x04, 0x00, 0x9a, 0x6f, 0x00, 0x00);
static const ble_uuid128_t kProvUuid =
    BLE_UUID128_INIT(0x9a, 0x6f, 0x21, 0x0c, 0x7d, 0x4e, 0x11, 0xa3,
                     0x4b, 0x5e, 0x05, 0x00, 0x9a, 0x6f, 0x00, 0x00);
static const ble_uuid128_t kLinkUuid =
    BLE_UUID128_INIT(0x9a, 0x6f, 0x21, 0x0c, 0x7d, 0x4e, 0x11, 0xa3,
                     0x4b, 0x5e, 0x06, 0x00, 0x9a, 0x6f, 0x00, 0x00);
static const ble_uuid128_t kTokenUuid =
    BLE_UUID128_INIT(0x9a, 0x6f, 0x21, 0x0c, 0x7d, 0x4e, 0x11, 0xa3,
                     0x4b, 0x5e, 0x07, 0x00, 0x9a, 0x6f, 0x00, 0x00);

static uint8_t s_addr_type;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_state_handle;
static uint16_t s_link_handle;
// Whether the central asked for state notifications, and whether the link is
// encrypted. Both have to be true before anything is pushed.
//
// ble_gatts_notify_custom sends regardless of either, which is what it is for
// — and what made this wrong. The panel notified its state four times a second
// down an unencrypted link to a central that had never subscribed, and those
// unsolicited values were the first thing the Mac saw succeed. The helper read
// that as "the link works, so it must be paired" and walked straight past the
// passkey into a provisioning write that could only be refused.
//
// So: nothing leaves here until somebody has asked for it over a link that is
// actually encrypted. That is also the correct posture on its own terms — the
// state characteristic is READ_ENC, and notifying it in the clear handed out
// exactly what the flag exists to protect.
static bool s_state_subscribed;
static bool s_encrypted;
// What the connected host calls itself, and the token minted for it. Both are
// per-connection: a name means nothing after the host has gone, and a token
// minted once must not be minted again on a reconnect.
static char s_peer_name[20] = {0};
static char s_conn_token[33] = {0};

// The same deliberately small string parser the HTTP side uses. One JSON
// object of three keys does not justify linking cJSON on either transport.
static bool ble_json_str(const char* body, const char* key, char* out, size_t cap) {
  const char* p = strstr(body, key);
  if (!p) return false;
  p = strchr(p + strlen(key), ':');
  if (!p) return false;
  while (*p && *p != '"') ++p;
  if (*p != '"') return false;
  ++p;
  size_t n = 0;
  while (*p && *p != '"' && n + 1 < cap) {
    if (*p == '\\' && p[1]) {
      ++p;
      char c = *p;
      if (c == 'n') c = '\n';
      else if (c == 't') c = '\t';
      out[n++] = c;
      ++p;
      continue;
    }
    out[n++] = *p++;
  }
  if (*p != '"') return false;
  out[n] = '\0';
  return true;
}
static char s_name[24] = "Pixelbar";

// The six digits currently on the panel, or 0 for "not pairing".
//
// Displaying the passkey is what makes this worth doing. Just Works bonding
// encrypts the link and stops a passer-by writing to it, but it authenticates
// nobody: anything in range can bond and then it is trusted for good. A code
// that only exists on a panel in front of you means the thing you paired with
// is the thing you are looking at — which is the entire question being asked,
// and a 24x8 display is unusually well suited to answering it.
static uint32_t s_passkey = 0;

uint32_t ble_passkey(void) { return s_passkey; }

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

// A scan is asked for by reading this: the read returns whatever the last one
// found, and starts a fresh one in the background. So the first read of a
// session is usually empty and the second, a couple of seconds later, is not.
// Blocking here instead would stall the Bluetooth host task for the two
// seconds the scan needs — on the radio the scan is using.
static int on_scan(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt* ctxt,
                   void* arg) {
  const char* j = net_scan_json();
  net_request_scan();
  return os_mbuf_append(ctxt->om, j, strlen(j)) == 0
             ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

// {"ssid":"…","pass":"…","name":"Nilava's Mac"} — credentials and who is
// sending them, in one write, because they are one intention.
static int on_prov(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt* ctxt,
                   void* arg) {
  char body[192];
  uint16_t got = 0;
  if (ble_hs_mbuf_to_flat(ctxt->om, body, sizeof(body) - 1, &got) != 0)
    return BLE_ATT_ERR_UNLIKELY;
  body[got] = '\0';

  char ssid[33] = {0}, pass[65] = {0};

  // The name is taken whether or not credentials came with it, so a host can
  // introduce itself, collect a token, and leave the network alone.
  ble_json_str(body, "\"name\"", s_peer_name, sizeof(s_peer_name));

  if (!ble_json_str(body, "\"ssid\"", ssid, sizeof(ssid)) || ssid[0] == '\0')
    return 0;
  // An absent password is an open network, not an error.
  ble_json_str(body, "\"pass\"", pass, sizeof(pass));
  net_provision(ssid, pass);
  return 0;
}

static int on_link(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt* ctxt,
                   void* arg) {
  char buf[320];
  const int n = net_link_json(buf, sizeof(buf));
  return os_mbuf_append(ctxt->om, buf, n) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

// The API token, handed across a link that is already authenticated.
//
// Minted once per connection and cached: a read that minted every time would
// fill the eight client slots with copies of one host that simply asked twice.
static int on_token(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt* ctxt,
                    void* arg) {
  if (s_conn_token[0] == '\0') {
    const char* t = NULL;
    if (!auth_issue(s_peer_name[0] ? s_peer_name : "bluetooth host", &t))
      return BLE_ATT_ERR_INSUFFICIENT_RES;  // no room; the panel says who holds it
    snprintf(s_conn_token, sizeof(s_conn_token), "%s", t);
  }
  char buf[96];
  const int n = snprintf(buf, sizeof(buf), "{\"token\":\"%s\",\"ip\":\"%s\"}",
                         s_conn_token, net_ip());
  return os_mbuf_append(ctxt->om, buf, n) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static const struct ble_gatt_svc_def kServices[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &kSvcUuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                // Write *with* response, and only with response.
                //
                // This carried WRITE_NO_RSP as well, on the reasoning that a
                // status push is fire-and-forget and an acknowledgement would
                // double its latency. That reasoning cost the entire pairing
                // flow. An ATT Write Command has no response PDU, so when the
                // server rejects it for insufficient authentication there is
                // nothing to carry the error back — the central never learns
                // that pairing is required and never starts it. macOS prefers
                // the unacknowledged form whenever a characteristic offers it,
                // so the write that was supposed to provoke pairing was
                // silently dropped on the floor every time.
                //
                // An unacknowledged write has no business on a characteristic
                // whose rejection is meaningful.
                .uuid = &kCmdUuid.u,
                .access_cb = on_cmd,
                // ENC and AUTHEN, not plain WRITE. Without these the panel is
                // controllable by anything within about ten metres, which is a
                // different security posture from the HTTP API on a home LAN
                // and a worse one: a LAN has a door on it.
                //
                // AUTHEN is the half that matters. ENC alone is satisfied by
                // Just Works, which encrypts against eavesdropping and
                // authenticates nobody.
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC |
                         BLE_GATT_CHR_F_WRITE_AUTHEN,
            },
            {
                .uuid = &kScanUuid.u,
                .access_cb = on_scan,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC |
                         BLE_GATT_CHR_F_READ_AUTHEN,
            },
            {
                .uuid = &kProvUuid.u,
                .access_cb = on_prov,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC |
                         BLE_GATT_CHR_F_WRITE_AUTHEN,
            },
            {
                .uuid = &kLinkUuid.u,
                .access_cb = on_link,
                .val_handle = &s_link_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY |
                         BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_READ_AUTHEN,
            },
            {
                .uuid = &kTokenUuid.u,
                .access_cb = on_token,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC |
                         BLE_GATT_CHR_F_READ_AUTHEN,
            },
            {
                .uuid = &kStateUuid.u,
                .access_cb = on_state,
                .val_handle = &s_state_handle,
                // Encrypted, but not authenticated. What this returns is what
                // the panel is already displaying to the room, so guarding it
                // as heavily as the ability to *change* it would be paying a
                // pairing prompt to keep a secret that is painted on the wall.
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY |
                         BLE_GATT_CHR_F_READ_ENC,
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
        // Ask for a slower connection, immediately.
        //
        // A central picks the interval, and macOS picks an aggressive one for
        // latency it assumes you want. Measured: an idle BLE connection at
        // whatever CoreBluetooth chose took this panel from 100 fps to 63 —
        // a connection *event* is a slot on a radio shared with WiFi, and on a
        // single-core chip the cost lands squarely in the frame budget.
        //
        // 150-300 ms is far slower than the default and far faster than
        // anything here needs: the traffic is a status push when a microphone
        // opens and a state notify four times a second. Nobody perceives a
        // quarter-second on either.
        struct ble_gap_upd_params p = {
            .itvl_min = 120,   // x1.25 ms
            .itvl_max = 240,
            .latency = 0,
            // Comfortably more than (1 + latency) x itvl_max x 2, which is the
            // rule that makes a link stable rather than one that keeps dropping.
            .supervision_timeout = 400,  // x10 ms
        };
        ble_gap_update_params(ev->connect.conn_handle, &p);
      } else {
        advertise();
      }
      return 0;
    case BLE_GAP_EVENT_DISCONNECT:
      s_conn = BLE_HS_CONN_HANDLE_NONE;
      // Also here, not only on ENC_CHANGE. A central that gives up part way
      // through — which is what cancelling the macOS dialog does — disconnects
      // without ever completing the exchange, and the panel was left showing a
      // code that would never be accepted and would never go away.
      s_passkey = 0;
      s_peer_name[0] = '\0';
      s_conn_token[0] = '\0';
      s_state_subscribed = false;
      s_encrypted = false;
      ESP_LOGI(TAG, "disconnected (%d)", ev->disconnect.reason);
      advertise();
      return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
      advertise();
      return 0;

    case BLE_GAP_EVENT_PASSKEY_ACTION: {
      if (ev->passkey.params.action != BLE_SM_IOACT_DISP) return 0;
      // Six digits from the hardware random number generator, not from a
      // counter and not from the clock. esp_random is fed by the radio's noise
      // and is exactly what this is for; a predictable code is not a code.
      struct ble_sm_io io = {.action = BLE_SM_IOACT_DISP};
      io.passkey = esp_random() % 1000000u;
      s_passkey = io.passkey ? io.passkey : 1;  // 0 means "not pairing"
      ESP_LOGI(TAG, "pairing: show %06lu on the panel", (unsigned long)s_passkey);
      ble_sm_inject_io(ev->passkey.conn_handle, &io);
      return 0;
    }

    case BLE_GAP_EVENT_ENC_CHANGE:
      // Pairing finished, one way or the other. The code comes off the panel
      // either way: a failed attempt should not leave a number on display that
      // someone could still type.
      s_passkey = 0;
      s_encrypted = ev->enc_change.status == 0;
      if (s_encrypted) {
        ESP_LOGI(TAG, "link security: established");
      } else {
        // The status is worth printing rather than flattening to "refused".
        // A host holding keys for a panel that has since been factory reset
        // fails here and nowhere else, and it is indistinguishable from a
        // mistyped code unless the number is shown.
        ESP_LOGW(TAG, "link security: refused (%d) — if this host paired "
                      "before a reset, forget the panel on it first",
                 ev->enc_change.status);
      }
      return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
      if (ev->subscribe.attr_handle == s_state_handle) {
        s_state_subscribed = ev->subscribe.cur_notify != 0;
      }
      return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
      // A central that bonded before and is presenting itself again. Drop the
      // old bond and let it pair afresh rather than refusing: the alternative
      // is a Mac that was re-imaged being permanently unable to reconnect,
      // with nothing on the device to clear it from.
      {
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(ev->repeat_pairing.conn_handle, &desc) == 0)
          ble_store_util_delete_peer(&desc.peer_id_addr);
      }
      return BLE_GAP_REPEAT_PAIRING_RETRY;
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
  // The device can show a number and cannot take one in. That is precisely
  // DisplayOnly, and it is what selects passkey-display pairing over Just
  // Works: the central has a keyboard, the panel has a display, and between
  // them that is enough for authenticated pairing.
  ble_hs_cfg.sm_io_cap = BLE_HS_IO_DISPLAY_ONLY;
  ble_hs_cfg.sm_bonding = 1;
  ble_hs_cfg.sm_mitm = 1;   // the half Just Works does not give you
  ble_hs_cfg.sm_sc = 1;     // Secure Connections: ECDH rather than legacy
  ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

  // Bonds live in NVS — and now that is actually true.
  //
  // This line was missing and the comment above it claimed otherwise. Without
  // it, ble_hs_cfg.store_read_cb / write_cb / delete_cb are all NULL and every
  // store operation returns BLE_HS_ENOTSUP. NimBLE discards the return value of
  // ble_store_write_*, so a first pairing *appeared* to succeed — the link
  // encrypted, ENC_CHANGE reported "established" — and was forgotten the
  // instant it completed. The central kept its half of the keys, so every
  // subsequent connection failed encryption against a device that had never
  // heard of it, and the repeat-pairing escape hatch was a no-op for the same
  // reason. Nothing in the ESP-IDF port calls this for you: it is invoked from
  // a Mynewt sysinit stage that this port does not run.
  ble_store_config_init();

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

// Forget every bonded host.
//
// The bond store is the authority on who may drive this panel over Bluetooth,
// so this is the Bluetooth half of a factory reset. Any host that was paired
// will also need to forget the panel on its own side — macOS keeps its half of
// the keys and will not re-run the passkey ceremony until it does.
void ble_forget_all(void) {
  const int rc = ble_store_clear();
  ESP_LOGW(TAG, "forgot every bonded host (%d)", rc);
  // Drop whoever is connected: their link is authenticated against a bond that
  // no longer exists, and leaving it up would be leaving a door open that has
  // had its lock removed.
  if (s_conn != BLE_HS_CONN_HANDLE_NONE)
    ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
}

void ble_publish(void) {
  if (s_conn == BLE_HS_CONN_HANDLE_NONE || s_state_handle == 0) return;
  // See s_state_subscribed. Asked for, and encrypted — neither of which
  // ble_gatts_notify_custom checks on its own.
  if (!s_state_subscribed || !s_encrypted) return;
  char buf[440];
  const int n = net_state_json(buf, sizeof(buf));
  struct os_mbuf* om = ble_hs_mbuf_from_flat(buf, n);
  if (!om) return;
  // Failure here is ordinary: the central may not have subscribed, or may have
  // gone away between the check above and now.
  ble_gatts_notify_custom(s_conn, s_state_handle, om);
}
