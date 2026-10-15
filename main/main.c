/* espkb: BLE keyboard(s) -> USB HID keyboard bridge for ESP32-S3,
 * with an on-demand Wi-Fi web UI for typing from a phone.
 *
 * Button tap:      toggle Wi-Fi AP + web UI (LED blue while on)
 * Button hold 3s:  pair a new BLE keyboard for 60s (LED blinks magenta) */

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
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
    xTaskCreate(button_task, "button", 2048, NULL, 5, NULL);

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
