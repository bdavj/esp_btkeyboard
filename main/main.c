/* espkb: BLE keyboard(s) -> USB HID keyboard bridge for ESP32-S3,
 * with an on-demand Wi-Fi web UI for typing from a phone.
 *
 * Button tap:      toggle Wi-Fi AP + web UI (LED blue while on)
 * Button hold 3s:  pair a new BLE keyboard for 60s (LED blinks magenta) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_core_dump.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "espkb.h"

static const char *TAG = "main";

#define LONG_PRESS_MS 3000
#define PAIR_SECONDS  60

static QueueHandle_t s_evq;

void app_post(app_ev_t ev)
{
    xQueueSend(s_evq, &ev, 0);
}

static void button_task(void *arg)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_ESPKB_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = CONFIG_ESPKB_BUTTON_ACTIVE_LOW,
        .pull_down_en = !CONFIG_ESPKB_BUTTON_ACTIVE_LOW,
    };
    gpio_config(&io);

    int held_ms = 0;
    bool long_sent = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(20));
        bool pressed = gpio_get_level(CONFIG_ESPKB_BUTTON_GPIO) == !CONFIG_ESPKB_BUTTON_ACTIVE_LOW;
        if (pressed) {
            held_ms += 20;
            if (held_ms >= LONG_PRESS_MS && !long_sent) {
                long_sent = true;
                app_post(EV_BTN_LONG);
            }
        } else {
            if (held_ms >= 40 && !long_sent) app_post(EV_BTN_TAP);
            held_ms = 0;
            long_sent = false;
        }
    }
}

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_SW: return "software restart";
    case ESP_RST_PANIC: return "CRASH (panic)";
    case ESP_RST_INT_WDT: return "CRASH (interrupt watchdog)";
    case ESP_RST_TASK_WDT: return "CRASH (task watchdog)";
    case ESP_RST_WDT: return "CRASH (other watchdog)";
    case ESP_RST_BROWNOUT: return "BROWNOUT (supply voltage dipped)";
    case ESP_RST_USB: return "USB reset";
    case ESP_RST_DEEPSLEEP: return "deep sleep wake";
    default: return "other";
    }
}

/* The log goes to USB serial, which the host only opens a moment after we
 * enumerate, so report why we last reset a few seconds after boot. */
static char s_boot_report[320] = "boot report not ready\r\n";

const char *app_boot_report(void)
{
    return s_boot_report;
}

static void boot_report_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(4000));
    esp_reset_reason_t r = esp_reset_reason();
    ESP_LOGW(TAG, "last reset: %s (%d)", reset_reason_str(r), r);
    int n = snprintf(s_boot_report, sizeof(s_boot_report), "last_reset=%s\r\n", reset_reason_str(r));
    ESP_LOGI(TAG, "free internal heap %u, largest block %u, min ever %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));

    esp_core_dump_summary_t *sum = malloc(sizeof(*sum));
    if (sum && esp_core_dump_image_check() == ESP_OK && esp_core_dump_get_summary(sum) == ESP_OK) {
        ESP_LOGE(TAG, "previous crash in task '%s' at PC 0x%08lx, cause %lu, vaddr 0x%08lx", sum->exc_task,
                 (unsigned long)sum->exc_pc, (unsigned long)sum->ex_info.exc_cause,
                 (unsigned long)sum->ex_info.exc_vaddr);
        char bt[16 * 11 + 1] = {0};
        for (int i = 0; i < sum->exc_bt_info.depth && i < 16; i++) {
            sprintf(bt + strlen(bt), " 0x%08lx", (unsigned long)sum->exc_bt_info.bt[i]);
        }
        ESP_LOGE(TAG, "backtrace:%s%s", bt, sum->exc_bt_info.corrupted ? " (corrupted)" : "");
        snprintf(s_boot_report + n, sizeof(s_boot_report) - n, "crash task=%s pc=0x%08lx cause=%lu vaddr=0x%08lx bt=%s\r\n",
                 sum->exc_task, (unsigned long)sum->exc_pc, (unsigned long)sum->ex_info.exc_cause,
                 (unsigned long)sum->ex_info.exc_vaddr, bt);
        esp_core_dump_image_erase();
    }
    free(sum);
    vTaskDelete(NULL);
}

static void update_led(int64_t now, int64_t flash_until)
{
    if (ble_host_pairing()) {
        bool on = (now / 250000) % 2;
        led_set(on ? 255 : 0, 0, on ? 255 : 0);
    } else if (now < flash_until) {
        led_set(0, 255, 0);
    } else if (web_running()) {
        led_set(0, 0, 255);
    } else {
        led_set(0, 0, 0);
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_evq = xQueueCreate(8, sizeof(app_ev_t));
    led_init();
    kb_init();
    usb_kbd_init();
    ble_host_init();
    web_init();
    xTaskCreate(button_task, "button", 4096, NULL, 5, NULL);
    xTaskCreate(boot_report_task, "boot_report", 4096, NULL, 3, NULL);

    ESP_LOGI(TAG, "espkb running. Tap button for web UI, hold %ds to pair.", LONG_PRESS_MS / 1000);

    int64_t flash_until = 0;
    for (;;) {
        app_ev_t ev;
        if (xQueueReceive(s_evq, &ev, pdMS_TO_TICKS(50))) {
            switch (ev) {
            case EV_BTN_TAP:
                if (web_running()) {
                    web_stop();
                } else {
                    web_start();
                }
                break;
            case EV_WEB_ON:
                web_start();
                break;
            case EV_WEB_OFF:
                web_stop();
                break;
            case EV_BTN_LONG:
            case EV_PAIR:
                ble_host_start_pairing(PAIR_SECONDS);
                break;
            case EV_KB_CONNECTED:
                flash_until = esp_timer_get_time() + 300000;
                break;
            }
        }

        int64_t now = esp_timer_get_time();
#if CONFIG_ESPKB_WEB_TIMEOUT_MIN > 0
        if (web_running() && now - web_last_activity_us() > CONFIG_ESPKB_WEB_TIMEOUT_MIN * 60LL * 1000000) {
            ESP_LOGI(TAG, "web UI idle, turning Wi-Fi off");
            web_stop();
        }
#endif
        update_led(now, flash_until);
    }
}
