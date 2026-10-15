/* USB HID keyboard using TinyUSB.
 *
 * Exposes a single boot-protocol keyboard interface with no report IDs, so it
 * works in BIOS/UEFI setup screens as well as in an OS. Supports remote wakeup
 * so a key press on a BLE keyboard can wake a sleeping laptop. */

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "class/hid/hid_device.h"
#include "espkb.h"

static const char *TAG = "usb";

static const uint8_t s_report_desc[] = {TUD_HID_REPORT_DESC_KEYBOARD()};

#define CFG_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)

static const uint8_t s_cfg_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, CFG_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(0, 4, HID_ITF_PROTOCOL_KEYBOARD, sizeof(s_report_desc), 0x81, 8, 1),
};

static const tusb_desc_device_t s_dev_desc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0,
    .bDeviceSubClass = 0,
    .bDeviceProtocol = 0,
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
};

static QueueHandle_t s_txq;

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    return s_report_desc;
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
    if (type == HID_REPORT_TYPE_OUTPUT && bufsize >= 1) {
        ble_host_set_leds(buffer[0]);
    }
}

static bool has_keys(const uint8_t r[8])
{
    if (r[0]) return true;
    for (int i = 2; i < 8; i++) {
        if (r[i]) return true;
    }
    return false;
}

static void tx_task(void *arg)
{
    uint8_t r[8];
    for (;;) {
        xQueueReceive(s_txq, r, portMAX_DELAY);
        if (!tud_mounted()) continue;

        if (tud_suspended()) {
            /* Only a press should wake the host; releases while asleep are moot. */
            if (!has_keys(r)) continue;
            tud_remote_wakeup();
            for (int i = 0; i < 100 && tud_suspended(); i++) vTaskDelay(pdMS_TO_TICKS(10));
            if (tud_suspended()) continue;
        }

        for (int i = 0; i < 500 && tud_mounted() && !tud_hid_ready(); i++) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        if (!tud_hid_keyboard_report(0, r[0], &r[2])) {
            ESP_LOGW(TAG, "report dropped");
        }
    }
}

void usb_kbd_init(void)
{
    s_txq = xQueueCreate(128, 8);

    const tinyusb_config_t cfg = {
        .device_descriptor = &s_dev_desc,
        .string_descriptor = s_str_desc,
        .string_descriptor_count = sizeof(s_str_desc) / sizeof(s_str_desc[0]),
        .external_phy = false,
        .configuration_descriptor = s_cfg_desc,
    };
    ESP_ERROR_CHECK(tinyusb_driver_install(&cfg));
    xTaskCreate(tx_task, "usb_tx", 3072, NULL, 6, NULL);
    ESP_LOGI(TAG, "USB HID keyboard ready");
}

void usb_kbd_send(const uint8_t report[8])
{
    if (xQueueSend(s_txq, report, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "tx queue full");
    }
}

bool usb_kbd_mounted(void)
{
    return tud_mounted();
}

bool usb_kbd_suspended(void)
{
    return tud_suspended();
}
