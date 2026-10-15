/* BLE HID host: connects to BLE keyboards (HID over GATT) as a central.
 *
 * Flow per keyboard:
 *   scan -> connect -> encrypt/pair -> discover HID service, characteristics,
 *   CCCDs -> switch to Boot Protocol -> subscribe to input reports.
 *
 * Bonded keyboards are reconnected automatically whenever they advertise.
 * New keyboards are only accepted while pairing mode is active. */

#include <string.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "espkb.h"

static const char *TAG = "ble";

void ble_store_config_init(void);

#define UUID_HID_SVC         0x1812
#define UUID_PROTOCOL_MODE   0x2A4E
#define UUID_REPORT          0x2A4D
#define UUID_BOOT_KB_IN      0x2A22
#define UUID_BOOT_KB_OUT     0x2A32
#define UUID_CCCD            0x2902
#define APPEARANCE_HID_CAT   15 /* appearance >> 6 */

#define MAX_CHRS 24
#define NSLOTS CONFIG_ESPKB_MAX_KEYBOARDS

typedef struct {
    uint16_t def_handle;
    uint16_t val_handle;
    uint16_t cccd;
    uint16_t uuid;
    uint8_t props;
} chr_t;

typedef struct {
    bool used;
    bool ready;
    bool fresh; /* new pairing rather than a bonded reconnect */
    uint16_t conn;
    ble_addr_t addr;
    char name[32];
    uint16_t svc_start, svc_end;
    chr_t chrs[MAX_CHRS];
    int nchrs;
    int idx; /* cursor for chained descriptor discovery / subscription */
    bool has_boot_in;
    uint16_t boot_out;
} slot_t;

static slot_t s_slots[NSLOTS];
static SemaphoreHandle_t s_mx;
static uint8_t s_own_addr_type;
static bool s_synced;
static volatile bool s_connecting;
static char s_pending_name[32];
static bool s_pending_fresh;
static int64_t s_pair_until;
static volatile int s_passkey = -1;
static volatile uint8_t s_leds;

static int gap_event(struct ble_gap_event *ev, void *arg);
static void start_scan(void);

/* ---- helpers ---- */

static int slot_by_conn(uint16_t conn)
{
    for (int i = 0; i < NSLOTS; i++) {
        if (s_slots[i].used && s_slots[i].conn == conn) return i;
    }
    return -1;
}

static int slot_by_addr(const ble_addr_t *a)
{
    for (int i = 0; i < NSLOTS; i++) {
        if (s_slots[i].used && memcmp(s_slots[i].addr.val, a->val, 6) == 0) return i;
    }
    return -1;
}

static int free_slot(void)
{
    for (int i = 0; i < NSLOTS; i++) {
        if (!s_slots[i].used) return i;
    }
    return -1;
}

static bool is_bonded(const ble_addr_t *a)
{
    struct ble_store_key_sec key = {0};
    struct ble_store_value_sec val;
    key.peer_addr = *a;
    if (key.peer_addr.type >= BLE_ADDR_PUBLIC_ID) key.peer_addr.type -= BLE_ADDR_PUBLIC_ID;
    return ble_store_read_peer_sec(&key, &val) == 0;
}

static bool adv_is_hid(const struct ble_hs_adv_fields *f)
{
    for (int i = 0; i < f->num_uuids16; i++) {
        if (ble_uuid_u16(&f->uuids16[i].u) == UUID_HID_SVC) return true;
    }
    return f->appearance_is_present && (f->appearance >> 6) == APPEARANCE_HID_CAT;
}

bool ble_host_pairing(void)
{
    return s_pair_until && esp_timer_get_time() < s_pair_until;
}

static void slot_clear(int i)
{
    xSemaphoreTake(s_mx, portMAX_DELAY);
    memset(&s_slots[i], 0, sizeof(s_slots[i]));
    xSemaphoreGive(s_mx);
    kb_set_source(i, 0, NULL);
}

static void send_leds(slot_t *s)
{
    if (s->ready && s->boot_out) {
        uint8_t v = s_leds;
        ble_gattc_write_no_rsp_flat(s->conn, s->boot_out, &v, 1);
    }
}

/* ---- GATT setup chain ---- */

static void subscribe_next(int i);

static int on_subscribed(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr,
                         void *arg)
{
    int i = (intptr_t)arg;
    if (slot_by_conn(conn) != i) return 0;
    if (err->status != 0) ESP_LOGW(TAG, "[%d] subscribe failed: %d", i, err->status);
    s_slots[i].idx++;
    subscribe_next(i);
    return 0;
}

static void subscribe_next(int i)
{
    slot_t *s = &s_slots[i];
    for (; s->idx < s->nchrs; s->idx++) {
        chr_t *c = &s->chrs[s->idx];
        bool input = (c->uuid == UUID_BOOT_KB_IN || c->uuid == UUID_REPORT);
        if (input && c->cccd && (c->props & BLE_GATT_CHR_PROP_NOTIFY)) {
            static const uint8_t on[2] = {1, 0};
            if (ble_gattc_write_flat(s->conn, c->cccd, on, sizeof(on), on_subscribed,
                                     (void *)(intptr_t)i) == 0) {
                return;
            }
        }
    }
    s->ready = true;
    ESP_LOGI(TAG, "[%d] '%s' ready (boot input: %s)", i, s->name, s->has_boot_in ? "yes" : "no");
    send_leds(s);
    app_post(EV_KB_CONNECTED);
}

static void setup_reports(int i)
{
    slot_t *s = &s_slots[i];
    for (int k = 0; k < s->nchrs; k++) {
        chr_t *c = &s->chrs[k];
        if (c->uuid == UUID_PROTOCOL_MODE) {
            /* Boot protocol gives a fixed 8-byte report we can pass straight to USB. */
            static const uint8_t boot = 0;
            ble_gattc_write_no_rsp_flat(s->conn, c->val_handle, &boot, 1);
        } else if (c->uuid == UUID_BOOT_KB_IN) {
            s->has_boot_in = true;
        } else if (c->uuid == UUID_BOOT_KB_OUT) {
            s->boot_out = c->val_handle;
        }
    }
    s->idx = 0;
    subscribe_next(i);
}

static void disc_dsc_next(int i);

static int on_dsc(uint16_t conn, const struct ble_gatt_error *err, uint16_t chr_val_handle,
                  const struct ble_gatt_dsc *dsc, void *arg)
{
    int i = (intptr_t)arg;
    if (slot_by_conn(conn) != i) return 0;
    slot_t *s = &s_slots[i];

    if (err->status == 0) {
        if (ble_uuid_u16(&dsc->uuid.u) == UUID_CCCD) s->chrs[s->idx].cccd = dsc->handle;
        return 0;
    }
    s->idx++;
    disc_dsc_next(i);
    return 0;
}

static void disc_dsc_next(int i)
{
    slot_t *s = &s_slots[i];
    for (; s->idx < s->nchrs; s->idx++) {
        chr_t *c = &s->chrs[s->idx];
        if (!(c->props & BLE_GATT_CHR_PROP_NOTIFY)) continue;
        uint16_t end = (s->idx + 1 < s->nchrs) ? s->chrs[s->idx + 1].def_handle - 1 : s->svc_end;
        if (end <= c->val_handle) continue;
        if (ble_gattc_disc_all_dscs(s->conn, c->val_handle, end, on_dsc, (void *)(intptr_t)i) == 0) {
            return;
        }
    }
    setup_reports(i);
}

static int on_chr(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_chr *chr,
                  void *arg)
{
    int i = (intptr_t)arg;
    if (slot_by_conn(conn) != i) return 0;
    slot_t *s = &s_slots[i];

    if (err->status == 0) {
        if (s->nchrs < MAX_CHRS) {
            chr_t *c = &s->chrs[s->nchrs++];
            c->def_handle = chr->def_handle;
            c->val_handle = chr->val_handle;
            c->props = chr->properties;
            c->uuid = (chr->uuid.u.type == BLE_UUID_TYPE_16) ? ble_uuid_u16(&chr->uuid.u) : 0;
        }
        return 0;
    }
    if (err->status != BLE_HS_EDONE) {
        ESP_LOGW(TAG, "[%d] chr discovery failed: %d", i, err->status);
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }
    s->idx = 0;
    disc_dsc_next(i);
    return 0;
}

static int on_svc(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_svc *svc,
                  void *arg)
{
    int i = (intptr_t)arg;
    if (slot_by_conn(conn) != i) return 0;
    slot_t *s = &s_slots[i];

    if (err->status == 0) {
        /* Some devices have more than one HID service (keyboard + mouse...); take the first. */
        if (!s->svc_start) {
            s->svc_start = svc->start_handle;
            s->svc_end = svc->end_handle;
        }
        return 0;
    }
    if (!s->svc_start) {
        ESP_LOGW(TAG, "[%d] no HID service, disconnecting", i);
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }
    ble_gattc_disc_all_chrs(conn, s->svc_start, s->svc_end, on_chr, arg);
    return 0;
}

/* ---- GAP ---- */

static void handle_report(int i, uint16_t attr, struct os_mbuf *om)
{
    slot_t *s = &s_slots[i];
    if (!s->ready) return;

    uint16_t uuid = 0;
    for (int k = 0; k < s->nchrs; k++) {
        if (s->chrs[k].val_handle == attr) uuid = s->chrs[k].uuid;
    }
    uint8_t buf[8];
    uint16_t len = OS_MBUF_PKTLEN(om);
    if (len != 8) return;

    /* Prefer the boot keyboard report; otherwise assume an 8-byte report is a keyboard. */
    if (uuid == UUID_BOOT_KB_IN || (uuid == UUID_REPORT && !s->has_boot_in)) {
        os_mbuf_copydata(om, 0, 8, buf);
        kb_set_source(i, buf[0], &buf[2]);
    }
}

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    struct ble_gap_conn_desc desc;
    int i;

    switch (ev->type) {
    case BLE_GAP_EVENT_DISC: {
        const struct ble_gap_disc_desc *d = &ev->disc;
        bool connectable = d->event_type == BLE_HCI_ADV_RPT_EVTYPE_ADV_IND ||
                           d->event_type == BLE_HCI_ADV_RPT_EVTYPE_DIR_IND;
        if (!connectable || s_connecting || slot_by_addr(&d->addr) >= 0 || free_slot() < 0) return 0;

        struct ble_hs_adv_fields f;
        bool parsed = ble_hs_adv_parse_fields(&f, d->data, d->length_data) == 0;
        bool want = is_bonded(&d->addr);
        bool fresh = !want;
        if (!want && ble_host_pairing() && parsed && adv_is_hid(&f)) want = true;
        if (!want) return 0;

        s_pending_name[0] = 0;
        if (parsed && f.name_len) {
            int n = f.name_len < sizeof(s_pending_name) - 1 ? f.name_len : sizeof(s_pending_name) - 1;
            for (int k = 0; k < n; k++) {
                char c = f.name[k];
                s_pending_name[k] = (c == '"' || c == '\\' || c < 0x20) ? '?' : c;
            }
            s_pending_name[n] = 0;
        }
        ESP_LOGI(TAG, "connecting to %s %02x:%02x:%02x:%02x:%02x:%02x '%s'", fresh ? "new" : "bonded",
                 d->addr.val[5], d->addr.val[4], d->addr.val[3], d->addr.val[2], d->addr.val[1],
                 d->addr.val[0], s_pending_name);

        s_pending_fresh = fresh;
        ble_gap_disc_cancel();
        s_connecting = true;
        if (ble_gap_connect(s_own_addr_type, &d->addr, 10000, NULL, gap_event, NULL) != 0) {
            s_connecting = false;
            start_scan();
        }
        return 0;
    }

    case BLE_GAP_EVENT_DISC_COMPLETE:
        start_scan();
        return 0;

    case BLE_GAP_EVENT_CONNECT:
        s_connecting = false;
        if (ev->connect.status != 0) {
            ESP_LOGW(TAG, "connect failed: %d", ev->connect.status);
            start_scan();
            return 0;
        }
        i = free_slot();
        if (i < 0 || ble_gap_conn_find(ev->connect.conn_handle, &desc) != 0) {
            ble_gap_terminate(ev->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            start_scan();
            return 0;
        }
        xSemaphoreTake(s_mx, portMAX_DELAY);
        memset(&s_slots[i], 0, sizeof(s_slots[i]));
        s_slots[i].used = true;
        s_slots[i].conn = ev->connect.conn_handle;
        s_slots[i].addr = desc.peer_id_addr;
        s_slots[i].fresh = s_pending_fresh;
        strlcpy(s_slots[i].name, s_pending_name[0] ? s_pending_name : "keyboard", sizeof(s_slots[i].name));
        xSemaphoreGive(s_mx);

        /* HID characteristics require an encrypted link: pair or re-encrypt first. */
        ble_gap_security_initiate(ev->connect.conn_handle);
        start_scan();
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        i = slot_by_conn(ev->disconnect.conn.conn_handle);
        ESP_LOGI(TAG, "[%d] disconnected (reason 0x%x)", i, ev->disconnect.reason);
        if (i >= 0) slot_clear(i);
        start_scan();
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        i = slot_by_conn(ev->enc_change.conn_handle);
        if (i < 0) return 0;
        if (ev->enc_change.status != 0) {
            ESP_LOGW(TAG, "[%d] encryption failed: %d", i, ev->enc_change.status);
            /* The keyboard probably lost its bond with us; forget it so it can re-pair. */
            if (ble_gap_conn_find(ev->enc_change.conn_handle, &desc) == 0) {
                ble_store_util_delete_peer(&desc.peer_id_addr);
            }
            ble_gap_terminate(ev->enc_change.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }
        if (s_slots[i].fresh) {
            s_passkey = -1;
            s_pair_until = 0; /* one new keyboard per pairing session */
        }
        ESP_LOGI(TAG, "[%d] encrypted, discovering HID service", i);
        ble_gattc_disc_svc_by_uuid(ev->enc_change.conn_handle, BLE_UUID16_DECLARE(UUID_HID_SVC), on_svc,
                                   (void *)(intptr_t)i);
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        if (ble_gap_conn_find(ev->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        struct ble_sm_io pk = {.action = ev->passkey.params.action};
        if (pk.action == BLE_SM_IOACT_DISP) {
            pk.passkey = esp_random() % 1000000;
            s_passkey = pk.passkey;
            ESP_LOGW(TAG, "PAIRING: type %06lu then Enter on the keyboard", (unsigned long)pk.passkey);
        } else if (pk.action == BLE_SM_IOACT_NUMCMP) {
            pk.numcmp_accept = 1;
        } else {
            ESP_LOGW(TAG, "unsupported pairing action %d", pk.action);
            return 0;
        }
        ble_sm_inject_io(ev->passkey.conn_handle, &pk);
        return 0;
    }

    case BLE_GAP_EVENT_NOTIFY_RX:
        i = slot_by_conn(ev->notify_rx.conn_handle);
        if (i >= 0 && !ev->notify_rx.indication) handle_report(i, ev->notify_rx.attr_handle, ev->notify_rx.om);
        return 0;

    default:
        return 0;
    }
}

static void start_scan(void)
{
    if (!s_synced || s_connecting || ble_gap_disc_active() || free_slot() < 0) return;

    struct ble_gap_disc_params p = {
        .itvl = 0x60,   /* 60ms */
        .window = 0x30, /* 30ms - leave airtime for Wi-Fi */
        .passive = 0,
        .filter_duplicates = 0,
    };
    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) ESP_LOGW(TAG, "scan start failed: %d", rc);
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    s_synced = true;
    ESP_LOGI(TAG, "BLE ready, scanning for bonded keyboards");
    start_scan();
}

static void on_reset(int reason)
{
    s_synced = false;
    ESP_LOGW(TAG, "host reset: %d", reason);
}

static void host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ---- public API ---- */

void ble_host_init(void)
{
    s_mx = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(nimble_port_init());

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
#if CONFIG_ESPKB_BLE_PASSKEY
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_DISP_ONLY;
#else
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
#endif
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_device_name_set("espkb");
    ble_store_config_init();
    nimble_port_freertos_init(host_task);
}

void ble_host_start_pairing(int seconds)
{
    s_passkey = -1;
    s_pair_until = esp_timer_get_time() + (int64_t)seconds * 1000000;
    ESP_LOGI(TAG, "pairing mode for %ds - put your keyboard into pairing mode", seconds);
    start_scan();
}

void ble_host_forget_all(void)
{
    for (int i = 0; i < NSLOTS; i++) {
        if (s_slots[i].used) ble_gap_terminate(s_slots[i].conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    ble_store_clear();
    ESP_LOGI(TAG, "all bonds deleted");
}

void ble_host_set_leds(uint8_t leds)
{
    s_leds = leds;
    for (int i = 0; i < NSLOTS; i++) {
        if (s_slots[i].used) send_leds(&s_slots[i]);
    }
}

void ble_host_status(ble_status_t *out)
{
    memset(out, 0, sizeof(*out));
    xSemaphoreTake(s_mx, portMAX_DELAY);
    for (int i = 0; i < NSLOTS; i++) {
        if (!s_slots[i].used) continue;
        strlcpy(out->kb[out->count].name, s_slots[i].name, sizeof(out->kb[0].name));
        out->kb[out->count].ready = s_slots[i].ready;
        out->count++;
    }
    xSemaphoreGive(s_mx);
    out->pairing = ble_host_pairing();
    out->passkey = out->pairing ? s_passkey : -1;
}
