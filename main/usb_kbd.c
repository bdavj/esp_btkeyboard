/* USB HID keyboard using TinyUSB.
 *
 * Exposes a boot-protocol keyboard interface with no report IDs, so it works
 * in BIOS/UEFI setup screens as well as in an OS, plus a second HID interface
 * for consumer control (media keys: volume, play/pause...). Supports remote
 * wakeup so a key press on a BLE keyboard can wake a sleeping laptop.
 *
 * With CONFIG_ESPKB_USB_LOG, a CDC-ACM serial port is added alongside the
 * keyboard and the ESP log is redirected to it. */

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "class/hid/hid_device.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "soc/rtc_cntl_reg.h"
#if CONFIG_ESPKB_USB_LOG
#include "tusb_cdc_acm.h"
#include "tusb_console.h"
#endif
#include "espkb.h"

static const char *TAG = "usb";

/* HID instance numbers (TinyUSB assigns them in interface order). The
 * keyboard must stay instance 0 / interface 0 with no report ID for BIOSes. */
enum { HID_KBD, HID_CONSUMER };
_Static_assert(CONFIG_TINYUSB_HID_COUNT >= 2, "set CONFIG_TINYUSB_HID_COUNT=2 for the consumer-control interface");

static const uint8_t s_report_desc_kbd[] = {TUD_HID_REPORT_DESC_KEYBOARD()};
static const uint8_t s_report_desc_consumer[] = {TUD_HID_REPORT_DESC_CONSUMER()};

#if CONFIG_ESPKB_USB_LOG
enum { ITF_HID, ITF_HID_CONSUMER, ITF_CDC, ITF_CDC_DATA, ITF_COUNT };
#define CFG_LEN (TUD_CONFIG_DESC_LEN + 2 * TUD_HID_DESC_LEN + TUD_CDC_DESC_LEN)
#else
enum { ITF_HID, ITF_HID_CONSUMER, ITF_COUNT };
#define CFG_LEN (TUD_CONFIG_DESC_LEN + 2 * TUD_HID_DESC_LEN)
#endif

static const uint8_t s_cfg_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, CFG_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(ITF_HID, 4, HID_ITF_PROTOCOL_KEYBOARD, sizeof(s_report_desc_kbd), 0x81, 8, 1),
    TUD_HID_DESCRIPTOR(ITF_HID_CONSUMER, 6, HID_ITF_PROTOCOL_NONE, sizeof(s_report_desc_consumer), 0x84, 2, 8),
#if CONFIG_ESPKB_USB_LOG
    TUD_CDC_DESCRIPTOR(ITF_CDC, 5, 0x82, 8, 0x03, 0x83, 64),
#endif
};

static const tusb_desc_device_t s_dev_desc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
#if CONFIG_ESPKB_USB_LOG
    /* Composite device with an interface association (CDC). */
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
#else
    .bDeviceClass = 0,
    .bDeviceSubClass = 0,
    .bDeviceProtocol = 0,
#endif
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = CONFIG_ESPKB_USB_VID,
    .idProduct = CONFIG_ESPKB_USB_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

static const char *s_str_desc[] = {
    (const char[]){0x09, 0x04}, /* English */
    "espkb",
    "espkb BLE-to-USB keyboard",
    "000001",
    "Keyboard",
    "espkb log",
    "Consumer control",
};

/* One queued report: which HID instance it is for, and its payload (8 bytes
 * for the keyboard, a 16-bit usage ID for consumer control). */
typedef struct {
    uint8_t itf;
    uint8_t data[8];
} tx_item_t;

static QueueHandle_t s_txq;
static TaskHandle_t s_tx_task;
static volatile uint32_t s_n_queued, s_n_sent, s_n_refused, s_n_suspended, s_n_qfull;

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    return instance == HID_CONSUMER ? s_report_desc_consumer : s_report_desc_kbd;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t type,
                               uint8_t *buffer, uint16_t reqlen)
{
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t type,
                           uint8_t const *buffer, uint16_t bufsize)
{
    /* Host -> keyboard LED state (Num/Caps/Scroll lock). Forward to BLE keyboards. */
    if (instance == HID_KBD && type == HID_REPORT_TYPE_OUTPUT && bufsize >= 1) {
        ble_host_set_leds(buffer[0]);
    }
}

static bool has_keys(const tx_item_t *it)
{
    const uint8_t *r = it->data;
    if (it->itf == HID_CONSUMER) return r[0] || r[1];
    if (r[0]) return true;
    for (int i = 2; i < 8; i++) {
        if (r[i]) return true;
    }
    return false;
}

static bool send_item(const tx_item_t *it)
{
    if (it->itf == HID_CONSUMER) return tud_hid_n_report(HID_CONSUMER, 0, it->data, 2);
    return tud_hid_keyboard_report(0, it->data[0], &it->data[2]);
}

/* tx_task sleeps until something changes: a report is queued, the host has
 * collected the previous report (tud_hid_report_complete_cb), or the bus is
 * mounted/resumed. The next report goes out the moment the IN endpoint is
 * free, so typing runs at the host's poll rate instead of a fixed delay. */
#define TX_RETRY_MS      50
#define TX_QUEUE_LEN     32
#define TX_QUEUE_WAIT_MS 500

static void tx_notify(void)
{
    if (s_tx_task) xTaskNotifyGive(s_tx_task);
}

void tud_hid_report_complete_cb(uint8_t instance, uint8_t const *report, uint16_t len)
{
    tx_notify();
}

void tud_mount_cb(void)
{
    tx_notify();
}

void tud_resume_cb(void)
{
    tx_notify();
}

static void tx_task(void *arg)
{
    /* Reports go out in order, one per queue item. If one can't be delivered
     * (host asleep, endpoint busy, cable yanked), we keep the latest desired
     * state and retry until the host has it, so a key never stays stuck down.
     * Intermediate states may be skipped in that case. */
    tx_item_t latest = {0};
    bool dirty = false, woke = false;
    for (;;) {
        if (!dirty && xQueueReceive(s_txq, &latest, 0) == pdTRUE) {
            dirty = true;
            s_n_queued++;
        }
        if (dirty && tud_mounted()) {
            if (tud_suspended()) {
                s_n_suspended++;
                /* Only a key press should wake the host; a release waits for resume. */
                if (has_keys(&latest) && !woke) {
                    tud_remote_wakeup();
                    woke = true;
                }
            } else {
                woke = false;
                if (tud_hid_n_ready(latest.itf)) {
                    if (send_item(&latest)) {
                        dirty = false;
                        s_n_sent++;
                        continue; /* pull the next report; it waits for the completion */
                    }
                    s_n_refused++;
                    ESP_LOGW(TAG, "report not accepted, retrying");
                }
            }
        }
        /* Wake on queue/completion/bus events; while a report is pending also
         * poll occasionally in case an event was missed. */
        ulTaskNotifyTake(pdTRUE, dirty ? pdMS_TO_TICKS(TX_RETRY_MS) : portMAX_DELAY);
    }
}

void usb_kbd_reboot_to_download(void)
{
    ESP_LOGW(TAG, "rebooting into download mode");
    vTaskDelay(pdMS_TO_TICKS(100));
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_restart();
}

#if CONFIG_ESPKB_USB_LOG
/* Opening the serial port at 1200 baud reboots into download mode (the
 * Arduino "1200 baud touch"), so the board can be reflashed without buttons. */
static void on_line_coding(int itf, cdcacm_event_t *ev)
{
    if (ev->line_coding_changed_data.p_line_coding->bit_rate == 1200) usb_kbd_reboot_to_download();
}

/* Single-character commands typed into the serial port. */
static void cdc_print(int itf, const char *str)
{
    tinyusb_cdcacm_write_queue(itf, (const uint8_t *)str, strlen(str));
    tinyusb_cdcacm_write_flush(itf, pdMS_TO_TICKS(50));
}

static void on_rx(int itf, cdcacm_event_t *ev)
{
    uint8_t buf[16];
    size_t n = 0;
    if (tinyusb_cdcacm_read(itf, buf, sizeof(buf), &n) != ESP_OK) return;
    for (size_t i = 0; i < n; i++) {
        switch (buf[i]) {
        case 's': {
            char line[160];
            ble_status_t st;
            ble_host_status(&st);
            snprintf(line, sizeof(line), "ver=%s slot=%s up=%llus usb=%d wifi=%d sta=%d joins=%d leaves=%d last_leave_reason=%d kb=%d heap=%u\r\n",
                     esp_app_get_description()->version, esp_ota_get_running_partition()->label,
                     (unsigned long long)(esp_timer_get_time() / 1000000), tud_mounted(), web_running(),
                     web_sta_count(), web_join_count(), web_leave_count(), web_last_leave_reason(), st.count,
                     (unsigned)esp_get_free_heap_size());
            cdc_print(itf, line);
            snprintf(line, sizeof(line), "hid: queued=%lu sent=%lu refused=%lu suspended_waits=%lu queue_full=%lu ready=%d consumer_ready=%d\r\n",
                     (unsigned long)s_n_queued, (unsigned long)s_n_sent, (unsigned long)s_n_refused,
                     (unsigned long)s_n_suspended, (unsigned long)s_n_qfull, tud_hid_n_ready(HID_KBD),
                     tud_hid_n_ready(HID_CONSUMER));
            cdc_print(itf, line);
            {
                static char dbg[1024];
                ble_host_debug(dbg, sizeof(dbg));
                cdc_print(itf, dbg);
            }
            cdc_print(itf, app_boot_report());
            break;
        }
        case 'w': app_post(EV_WEB_ON); break;
        case 'x': app_post(EV_WEB_OFF); break;
        case 'p': app_post(EV_PAIR); break;
        case 'B': usb_kbd_reboot_to_download(); break;
        case 'h':
        case '?': cdc_print(itf, "espkb: s=status w=Wi-Fi on x=Wi-Fi off p=pair B=download mode\r\n"); break;
        }
    }
}
#endif

void usb_kbd_init(void)
{
    s_txq = xQueueCreate(TX_QUEUE_LEN, sizeof(tx_item_t));

    const tinyusb_config_t cfg = {
        .device_descriptor = &s_dev_desc,
        .string_descriptor = s_str_desc,
        .string_descriptor_count = sizeof(s_str_desc) / sizeof(s_str_desc[0]),
        .external_phy = false,
        .configuration_descriptor = s_cfg_desc,
    };
    ESP_ERROR_CHECK(tinyusb_driver_install(&cfg));
#if CONFIG_ESPKB_USB_LOG
    const tinyusb_config_cdcacm_t acm = {
        .usb_dev = TINYUSB_USBDEV_0,
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = on_rx,
        .callback_line_coding_changed = on_line_coding,
    };
    ESP_ERROR_CHECK(tusb_cdc_acm_init(&acm));
#if CONFIG_ESPKB_USB_CONSOLE
    esp_tusb_init_console(TINYUSB_CDC_ACM_0);
#endif
#endif
    xTaskCreate(tx_task, "usb_tx", 4096, NULL, 6, &s_tx_task);
    ESP_LOGI(TAG, "USB HID keyboard ready");
}

static void enqueue(const tx_item_t *it)
{
    /* The queue drains at the host's poll rate (1-8 ms per report), so a wait
     * here only means a burst outran the host; keep the queue short so that
     * cancelling text typing doesn't leave much already queued. */
    if (xQueueSend(s_txq, it, pdMS_TO_TICKS(TX_QUEUE_WAIT_MS)) == pdTRUE) {
        tx_notify();
    } else {
        s_n_qfull++;
        ESP_LOGW(TAG, "tx queue full");
    }
}

void usb_kbd_send(const uint8_t report[8])
{
    tx_item_t it = {.itf = HID_KBD};
    memcpy(it.data, report, 8);
    enqueue(&it);
}

void usb_kbd_send_consumer(uint16_t usage)
{
    tx_item_t it = {.itf = HID_CONSUMER, .data = {usage & 0xFF, usage >> 8}};
    enqueue(&it);
}

bool usb_kbd_mounted(void)
{
    return tud_mounted();
}

bool usb_kbd_suspended(void)
{
    return tud_suspended();
}
