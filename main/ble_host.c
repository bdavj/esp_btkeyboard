/* BLE HID host: connects to BLE keyboards (HID over GATT) as a central.
 *
 * Flow per keyboard:
 *   scan -> connect -> encrypt/pair -> discover HID service, characteristics,
 *   descriptors -> read Report References and the Report Map -> pick Boot or
 *   Report protocol -> subscribe to input reports.
 *
 * Standard 8-byte keyboard reports are merged into the USB keyboard (keyboard.c);
 * consumer-control (media key) reports are decoded with the layout from the
 * Report Map and forwarded to the USB consumer-control interface.
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
#define UUID_REPORT_MAP      0x2A4B
#define UUID_REPORT_REF      0x2908
#define UUID_PROTOCOL_MODE   0x2A4E
#define UUID_REPORT          0x2A4D
#define UUID_BOOT_KB_IN      0x2A22
#define UUID_BOOT_KB_OUT     0x2A32
#define UUID_CCCD            0x2902
#define APPEARANCE_HID_CAT   15 /* appearance >> 6 */

#define MAX_CHRS 24
#define NSLOTS CONFIG_ESPKB_MAX_KEYBOARDS
#define REPORT_MAP_MAX 512

#define REPORT_TYPE_INPUT  1
#define REPORT_TYPE_OUTPUT 2

typedef struct {
    uint16_t def_handle;
    uint16_t val_handle;
    uint16_t cccd;
    uint16_t ref_dsc;    /* Report Reference descriptor handle (Report chrs) */
    uint16_t uuid;
    uint8_t props;
    uint8_t report_id;   /* from the Report Reference */
    uint8_t report_type; /* REPORT_TYPE_* or 0 if unknown */
} chr_t;

/* Where consumer-control usages live in the keyboard's input report, from
 * the Report Map. Two forms occur: a bitmap (one bit per usage, "variable")
 * and an array of usage IDs (usually one or two 16-bit slots). */
#define CC_MAX_BITS 32
#define CC_MAX_ARR  4
typedef struct {
    bool found;
    uint8_t report_id;
    uint8_t nbits, narr;
    struct {
        uint16_t bit;
        uint16_t usage;
    } bits[CC_MAX_BITS];
    struct {
        uint16_t bit;
        uint8_t size, count;
        uint16_t usage_min;
        int32_t logical_min;
    } arr[CC_MAX_ARR];
} cc_layout_t;

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
    bool report_mode; /* left in Report Protocol so media keys arrive */
    uint16_t boot_out;
    uint16_t map_len;
    uint8_t map[REPORT_MAP_MAX];
    bool kb_std_report; /* Report Map has a standard 8-byte keyboard input report */
    cc_layout_t cc;
    uint16_t cc_last; /* last consumer usage sent to USB (0 = released) */
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
static volatile bool s_wifi_active;

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
    bool cc_held = s_slots[i].cc_last != 0;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    memset(&s_slots[i], 0, sizeof(s_slots[i]));
    xSemaphoreGive(s_mx);
    kb_set_source(i, 0, NULL);
    if (cc_held) usb_kbd_send_consumer(0);
}

static void send_leds(slot_t *s)
{
    if (s->ready && s->boot_out) {
        uint8_t v = s_leds;
        ble_gattc_write_no_rsp_flat(s->conn, s->boot_out, &v, 1);
    }
}

/* ---- Report Map parsing ---- */

/* Walks the HID report descriptor and records (a) where consumer-control
 * usages sit in their input report and (b) whether there is a standard
 * boot-style keyboard input report (8 modifier bits + 6 key-code bytes). Only
 * what is needed to route reports; everything else is skipped. */
static void parse_report_map(slot_t *s)
{
    const uint8_t *d = s->map;
    uint16_t len = s->map_len;
    cc_layout_t *cc = &s->cc;
    memset(cc, 0, sizeof(*cc));
    s->kb_std_report = false;

    /* Global items */
    uint16_t usage_page = 0;
    uint8_t report_id = 0;
    uint32_t report_size = 0, report_count = 0;
    int32_t logical_min = 0;
    /* Local items (cleared after each main item) */
    uint32_t usages[CC_MAX_BITS];
    int nusages = 0;
    uint32_t usage_min = 0;
    bool have_min = false;
    /* Input bit position per report ID */
    struct {
        uint8_t rid;
        uint16_t bits;
    } pos[8];
    int npos = 0;

    uint16_t p = 0;
    while (p < len) {
        uint8_t prefix = d[p++];
        if (prefix == 0xFE) { /* long item: skip */
            if (p + 1 >= len) break;
            p += 2 + d[p];
            continue;
        }
        uint8_t size = prefix & 3;
        if (size == 3) size = 4;
        if (p + size > len) break;
        uint32_t v = 0;
        for (int k = 0; k < size; k++) v |= (uint32_t)d[p + k] << (8 * k);
        int32_t sv = v;
        if (size == 1) sv = (int8_t)v;
        else if (size == 2) sv = (int16_t)v;
        p += size;
        uint8_t tag = prefix & 0xFC;

        switch (tag) {
        /* global */
        case 0x04: usage_page = v; break;
        case 0x14: logical_min = sv; break;
        case 0x74: report_size = v; break;
        case 0x84: report_id = v; break;
        case 0x94: report_count = v; break;
        /* local */
        case 0x08:
            if (nusages < CC_MAX_BITS) usages[nusages++] = (size == 4) ? v : ((uint32_t)usage_page << 16 | v);
            break;
        case 0x18:
            usage_min = (size == 4) ? v : ((uint32_t)usage_page << 16 | v);
            have_min = true;
            break;
        /* main */
        case 0x80: { /* Input */
            int pi = -1;
            for (int k = 0; k < npos; k++) {
                if (pos[k].rid == report_id) pi = k;
            }
            if (pi < 0 && npos < 8) {
                pi = npos++;
                pos[pi].rid = report_id;
                pos[pi].bits = 0;
            }
            uint16_t bit = pi >= 0 ? pos[pi].bits : 0;
            bool constant = v & 1, variable = v & 2;
            bool consumer = usage_page == 0x0C || (nusages && (usages[0] >> 16) == 0x0C) ||
                            (have_min && (usage_min >> 16) == 0x0C);
            if (!constant && consumer && (!cc->found || cc->report_id == report_id)) {
                cc->found = true;
                cc->report_id = report_id;
                if (variable) {
                    for (uint32_t k = 0; k < report_count && cc->nbits < CC_MAX_BITS; k++) {
                        uint32_t u;
                        if (k < (uint32_t)nusages) u = usages[k];
                        else if (have_min) u = usage_min + k;
                        else if (nusages) u = usages[nusages - 1];
                        else break;
                        if ((u >> 16) != 0x0C) continue;
                        cc->bits[cc->nbits].bit = bit + k * report_size;
                        cc->bits[cc->nbits].usage = u & 0xFFFF;
                        cc->nbits++;
                    }
                } else if (cc->narr < CC_MAX_ARR && report_size <= 16) {
                    cc->arr[cc->narr].bit = bit;
                    cc->arr[cc->narr].size = report_size;
                    cc->arr[cc->narr].count = report_count > 8 ? 8 : report_count;
                    cc->arr[cc->narr].usage_min = have_min ? (usage_min & 0xFFFF) : 0;
                    cc->arr[cc->narr].logical_min = logical_min;
                    cc->narr++;
                }
            }
            if (!constant && !variable && usage_page == 0x07 && report_size == 8 && report_count == 6 && bit == 16) {
                s->kb_std_report = true;
            }
            if (pi >= 0) pos[pi].bits += report_size * report_count;
            break;
        }
        }
        if (tag == 0x80 || tag == 0x90 || tag == 0xB0 || tag == 0xA0 || tag == 0xC0) {
            nusages = 0;
            have_min = false;
            usage_min = 0;
        }
    }
}

/* Returns the first pressed consumer usage in a report, or 0. */
static uint16_t decode_consumer(const cc_layout_t *cc, const uint8_t *r, uint16_t len)
{
    uint32_t nbits_total = (uint32_t)len * 8;
    for (int k = 0; k < cc->nbits; k++) {
        uint16_t b = cc->bits[k].bit;
        if (b < nbits_total && (r[b / 8] >> (b % 8)) & 1) return cc->bits[k].usage;
    }
    for (int k = 0; k < cc->narr; k++) {
        for (int n = 0; n < cc->arr[k].count; n++) {
            uint32_t b = cc->arr[k].bit + (uint32_t)n * cc->arr[k].size;
            if (b + cc->arr[k].size > nbits_total) break;
            uint32_t v = 0;
            for (int q = 0; q < cc->arr[k].size; q++) {
                v |= (uint32_t)((r[(b + q) / 8] >> ((b + q) % 8)) & 1) << q;
            }
            if (v == 0 || (int32_t)v < cc->arr[k].logical_min) continue;
            return cc->arr[k].usage_min + (v - cc->arr[k].logical_min);
        }
    }
    return 0;
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
    uint16_t out_report = 0;

    /* Boot protocol gives a fixed 8-byte report we can pass straight to USB,
     * but a keyboard honouring it stops sending consumer (media key) reports.
     * When the Report Map shows a standard keyboard report alongside a
     * consumer report, stay in Report Protocol instead: the keyboard report
     * is the same 8 bytes and the media keys come through too. */
    s->report_mode = s->kb_std_report && s->cc.found;
    for (int k = 0; k < s->nchrs; k++) {
        chr_t *c = &s->chrs[k];
        if (c->uuid == UUID_PROTOCOL_MODE && !s->report_mode) {
            static const uint8_t boot = 0;
            ble_gattc_write_no_rsp_flat(s->conn, c->val_handle, &boot, 1);
        } else if (c->uuid == UUID_BOOT_KB_IN) {
            s->has_boot_in = true;
        } else if (c->uuid == UUID_BOOT_KB_OUT) {
            s->boot_out = c->val_handle;
        } else if (c->uuid == UUID_REPORT && c->report_type == REPORT_TYPE_OUTPUT && !out_report) {
            out_report = c->val_handle;
        }
    }
    if (s->report_mode && out_report) s->boot_out = out_report;
    ESP_LOGI(TAG, "[%d] report map %u bytes: std keyboard report=%d consumer report id=%d (%d bits, %d arrays) -> %s protocol",
             i, s->map_len, s->kb_std_report, s->cc.found ? s->cc.report_id : -1, s->cc.nbits, s->cc.narr,
             s->report_mode ? "report" : "boot");
    s->idx = 0;
    subscribe_next(i);
}

static int on_map(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    int i = (intptr_t)arg;
    if (slot_by_conn(conn) != i) return 0;
    slot_t *s = &s_slots[i];

    if (err->status == 0) {
        uint16_t n = OS_MBUF_PKTLEN(attr->om);
        if (s->map_len + n > REPORT_MAP_MAX) n = REPORT_MAP_MAX - s->map_len;
        os_mbuf_copydata(attr->om, 0, n, s->map + s->map_len);
        s->map_len += n;
        return 0;
    }
    if (err->status != BLE_HS_EDONE) ESP_LOGW(TAG, "[%d] report map read failed: %d", i, err->status);
    if (s->map_len) parse_report_map(s);
    setup_reports(i);
    return 0;
}

static void read_map(int i)
{
    slot_t *s = &s_slots[i];
    s->map_len = 0;
    for (int k = 0; k < s->nchrs; k++) {
        if (s->chrs[k].uuid == UUID_REPORT_MAP &&
            ble_gattc_read_long(s->conn, s->chrs[k].val_handle, 0, on_map, (void *)(intptr_t)i) == 0) {
            return;
        }
    }
    setup_reports(i);
}

static void read_refs_next(int i);

static int on_ref(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    int i = (intptr_t)arg;
    if (slot_by_conn(conn) != i) return 0;
    slot_t *s = &s_slots[i];
    chr_t *c = &s->chrs[s->idx];

    if (err->status == 0 && OS_MBUF_PKTLEN(attr->om) >= 2) {
        uint8_t ref[2];
        os_mbuf_copydata(attr->om, 0, 2, ref);
        c->report_id = ref[0];
        c->report_type = ref[1];
    } else if (err->status != 0) {
        ESP_LOGW(TAG, "[%d] report reference read failed: %d", i, err->status);
    }
    s->idx++;
    read_refs_next(i);
    return 0;
}

/* Reads the Report Reference (report ID + type) of every Report characteristic. */
static void read_refs_next(int i)
{
    slot_t *s = &s_slots[i];
    for (; s->idx < s->nchrs; s->idx++) {
        chr_t *c = &s->chrs[s->idx];
        if (c->ref_dsc && ble_gattc_read(s->conn, c->ref_dsc, on_ref, (void *)(intptr_t)i) == 0) return;
    }
    read_map(i);
}

static void disc_dsc_next(int i);

static int on_dsc(uint16_t conn, const struct ble_gatt_error *err, uint16_t chr_val_handle,
                  const struct ble_gatt_dsc *dsc, void *arg)
{
    int i = (intptr_t)arg;
    if (slot_by_conn(conn) != i) return 0;
    slot_t *s = &s_slots[i];

    if (err->status == 0) {
        uint16_t u = (dsc->uuid.u.type == BLE_UUID_TYPE_16) ? ble_uuid_u16(&dsc->uuid.u) : 0;
        if (u == UUID_CCCD) s->chrs[s->idx].cccd = dsc->handle;
        if (u == UUID_REPORT_REF) s->chrs[s->idx].ref_dsc = dsc->handle;
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
        /* Input reports (for the CCCD) and every Report (for its Report Reference). */
        if (!(c->props & BLE_GATT_CHR_PROP_NOTIFY) && c->uuid != UUID_REPORT) continue;
        uint16_t end = (s->idx + 1 < s->nchrs) ? s->chrs[s->idx + 1].def_handle - 1 : s->svc_end;
        if (end <= c->val_handle) continue;
        if (ble_gattc_disc_all_dscs(s->conn, c->val_handle, end, on_dsc, (void *)(intptr_t)i) == 0) {
            return;
        }
    }
    s->idx = 0;
    read_refs_next(i);
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

static volatile uint32_t s_n_notify, s_n_reports, s_n_consumer, s_n_rejected;
static volatile uint16_t s_last_uuid, s_last_len, s_last_cc;
static uint8_t s_last_data[8];

#define REJECT_LOG_MAX 20

static void reject(int i, const chr_t *c, const uint8_t *buf, uint16_t len, const char *why)
{
    static int logged;
    s_n_rejected++;
    if (logged >= REJECT_LOG_MAX) return;
    logged++;
    char hex[3 * 16 + 1] = "";
    int n = len < 16 ? len : 16;
    for (int k = 0; k < n; k++) snprintf(hex + 3 * k, sizeof(hex) - 3 * k, "%02x ", buf[k]);
    ESP_LOGI(TAG, "[%d] rejected (%s): chr 0x%04x handle %u report id %u type %u len %u: %s", i, why,
             c ? c->uuid : 0, c ? c->val_handle : 0, c ? c->report_id : 0, c ? c->report_type : 0, len, hex);
}

static void send_consumer(slot_t *s, int i, uint16_t usage)
{
    if (usage == s->cc_last) return;
    s->cc_last = usage;
    s_last_cc = usage;
    s_n_consumer++;
    ESP_LOGD(TAG, "[%d] consumer usage 0x%04x", i, usage);
    usb_kbd_send_consumer(usage);
}

static void handle_report(int i, uint16_t attr, struct os_mbuf *om)
{
    slot_t *s = &s_slots[i];
    if (!s->ready) return;

    const chr_t *c = NULL;
    for (int k = 0; k < s->nchrs; k++) {
        if (s->chrs[k].val_handle == attr) c = &s->chrs[k];
    }
    uint16_t uuid = c ? c->uuid : 0;
    uint16_t len = OS_MBUF_PKTLEN(om);
    s_n_notify++;
    s_last_uuid = uuid;
    s_last_len = len;

    uint8_t buf[16];
    os_mbuf_copydata(om, 0, len < sizeof(buf) ? len : sizeof(buf), buf);

    if (uuid != UUID_BOOT_KB_IN && uuid != UUID_REPORT) {
        reject(i, c, buf, len, "not an input report");
        return;
    }

    /* Consumer-control report: routed by report ID when the Report Map gave
     * us one, otherwise a short (1-3 byte) Report notification is taken as
     * the common single 16-bit usage form. */
    if (uuid == UUID_REPORT && c->report_type != REPORT_TYPE_OUTPUT) {
        bool by_id = s->cc.found && c->report_id == s->cc.report_id && (c->report_type == REPORT_TYPE_INPUT || !c->report_type);
        if (by_id && len > 0) {
            send_consumer(s, i, decode_consumer(&s->cc, buf, len));
            return;
        }
        if (!s->cc.found && len >= 1 && len <= 3) {
            uint16_t usage = buf[0] | (len > 1 ? (uint16_t)buf[1] << 8 : 0);
            send_consumer(s, i, usage);
            return;
        }
    }

    if (len < 8 || len > 16) {
        reject(i, c, buf, len, "length");
        return;
    }
    memcpy(s_last_data, buf, 8);

    /* Keyboards don't always honour the switch to boot protocol, so take a
     * standard keyboard report from either characteristic. Standard layout:
     * modifiers, reserved (0), six key codes (< 0xE8). Anything else (mouse,
     * NKRO bitmap, vendor) is ignored. */
    if (buf[1] != 0) {
        reject(i, c, buf, len, "reserved byte");
        return;
    }
    for (int k = 2; k < 8; k++) {
        if (buf[k] >= 0xE8) {
            reject(i, c, buf, len, "key code");
            return;
        }
    }
    s_n_reports++;
    kb_set_source(i, buf[0], &buf[2]);
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

    /* Units of 0.625ms. Keyboards advertise often when reconnecting, so even
     * the low duty cycle catches them within a second or two. */
    struct ble_gap_disc_params p = {
        .itvl = s_wifi_active ? 0x100 : 0x60,   /* 160ms : 60ms */
        .window = s_wifi_active ? 0x20 : 0x30,  /* 20ms (12%) : 30ms (50%) */
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
#if !CONFIG_ESPKB_BLE_ENABLE
    ESP_LOGW(TAG, "Bluetooth disabled in config");
    return;
#endif
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
    if (!s_synced) return;
    s_passkey = -1;
    s_pair_until = esp_timer_get_time() + (int64_t)seconds * 1000000;
    ESP_LOGI(TAG, "pairing mode for %ds - put your keyboard into pairing mode", seconds);
    start_scan();
}

void ble_host_set_wifi_active(bool on)
{
    s_wifi_active = on;
    if (!s_synced) return;
    if (ble_gap_disc_active()) {
        ble_gap_disc_cancel();
        start_scan();
    }
}

void ble_host_forget_all(void)
{
    if (!s_synced) return;
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

int ble_host_debug(char *buf, size_t size)
{
    int n = 0;
    n += snprintf(buf + n, size - n, "ble: synced=%d scanning=%d pairing=%d notify=%lu reports=%lu consumer=%lu rejected=%lu last_cc=0x%04x last_uuid=0x%04x last_len=%u last=%02x %02x %02x %02x %02x %02x %02x %02x\r\n",
                  s_synced, ble_gap_disc_active(), ble_host_pairing(), (unsigned long)s_n_notify,
                  (unsigned long)s_n_reports, (unsigned long)s_n_consumer, (unsigned long)s_n_rejected,
                  s_last_cc, s_last_uuid, s_last_len,
                  s_last_data[0], s_last_data[1], s_last_data[2], s_last_data[3], s_last_data[4],
                  s_last_data[5], s_last_data[6], s_last_data[7]);
    for (int i = 0; i < NSLOTS && n < size; i++) {
        slot_t *s = &s_slots[i];
        if (!s->used) continue;
        n += snprintf(buf + n, size - n, "  kb[%d] '%s' conn=%d ready=%d boot_in=%d mode=%s map=%u std_kb=%d cc_rid=%d cc_bits=%d cc_arr=%d chrs=%d:",
                      i, s->name, s->conn, s->ready, s->has_boot_in, s->report_mode ? "report" : "boot", s->map_len,
                      s->kb_std_report, s->cc.found ? s->cc.report_id : -1, s->cc.nbits, s->cc.narr, s->nchrs);
        for (int k = 0; k < s->nchrs && n < size; k++) {
            n += snprintf(buf + n, size - n, " %04x/p%02x/c%d/r%u.%u", s->chrs[k].uuid, s->chrs[k].props,
                          s->chrs[k].cccd != 0, s->chrs[k].report_id, s->chrs[k].report_type);
        }
        n += snprintf(buf + n, size - n, "\r\n");
    }
    return n;
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
