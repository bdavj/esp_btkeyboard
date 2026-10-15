/* Merges key state from all sources into one USB report, and types text. */

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "espkb.h"

static const char *TAG = "kb";

#define HID_KEY_ERR_ROLLOVER 0x01
#define TYPE_BUF_SIZE 8192

typedef struct {
    uint8_t mods;
    uint8_t keys[6];
} src_state_t;

static src_state_t s_src[KB_NSRC];
static uint8_t s_last[8];
static SemaphoreHandle_t s_mx;
static StreamBufferHandle_t s_type_buf;
static volatile bool s_type_cancel;

#define SPAM_PRESS_MS 40
#define SPAM_GAP_MS   40

static uint8_t s_spam_keys[KB_SPAM_MAX_KEYS];
static volatile int s_spam_n;
static volatile int64_t s_spam_until;
static TaskHandle_t s_spam_task;

/* Must hold s_mx. */
static void push_locked(void)
{
    uint8_t r[8] = {0};
    int n = 0;
    bool rollover = false;

    for (int s = 0; s < KB_NSRC; s++) {
        r[0] |= s_src[s].mods;
        for (int i = 0; i < 6; i++) {
            uint8_t k = s_src[s].keys[i];
            if (k == 0) continue;
            if (k == HID_KEY_ERR_ROLLOVER) {
                rollover = true;
                continue;
            }
            bool dup = false;
            for (int j = 0; j < n; j++) dup |= (r[2 + j] == k);
            if (dup) continue;
            if (n == 6) {
                rollover = true;
                continue;
            }
            r[2 + n++] = k;
        }
    }
    if (rollover) memset(&r[2], HID_KEY_ERR_ROLLOVER, 6);

    if (memcmp(r, s_last, sizeof(r)) != 0) {
        memcpy(s_last, r, sizeof(r));
        usb_kbd_send(r);
    }
}

void kb_set_source(int src, uint8_t mods, const uint8_t keys[6])
{
    if (src < 0 || src >= KB_NSRC) return;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    s_src[src].mods = mods;
    if (keys) {
        memcpy(s_src[src].keys, keys, 6);
    } else {
        memset(s_src[src].keys, 0, 6);
    }
    push_locked();
    xSemaphoreGive(s_mx);
}

void kb_web_key(bool down, uint8_t mods, uint8_t key)
{
    xSemaphoreTake(s_mx, portMAX_DELAY);
    src_state_t *w = &s_src[KB_SRC_WEB];
    if (down) {
        w->mods |= mods;
        bool present = false;
        for (int i = 0; i < 6; i++) present |= (w->keys[i] == key);
        if (key && !present) {
            for (int i = 0; i < 6; i++) {
                if (w->keys[i] == 0) {
                    w->keys[i] = key;
                    break;
                }
            }
        }
    } else {
        bool any = false;
        for (int i = 0; i < 6; i++) {
            if (key && w->keys[i] == key) w->keys[i] = 0;
            any |= (w->keys[i] != 0);
        }
        /* Modifiers sent with a key are dropped once no web keys are held. */
        if (!any) w->mods = 0;
    }
    push_locked();
    xSemaphoreGive(s_mx);
}

void kb_web_release(void)
{
    kb_set_source(KB_SRC_WEB, 0, NULL);
}

/* ---- text typing ---- */

static void tap(uint8_t mods, uint8_t key)
{
    uint8_t keys[6] = {key};
    kb_set_source(KB_SRC_TYPE, mods, keys);
    vTaskDelay(pdMS_TO_TICKS(CONFIG_ESPKB_TYPE_DELAY_MS));
    kb_set_source(KB_SRC_TYPE, 0, NULL);
    vTaskDelay(pdMS_TO_TICKS(CONFIG_ESPKB_TYPE_DELAY_MS));
}

static void type_task(void *arg)
{
    uint32_t cp = 0;
    int need = 0;
    bool last_cr = false;
    uint8_t c;

    for (;;) {
        xStreamBufferReceive(s_type_buf, &c, 1, portMAX_DELAY);
        if (s_type_cancel) {
            need = 0;
            continue;
        }

        /* Minimal UTF-8 decoder. */
        if (need == 0) {
            if (c < 0x80) {
                cp = c;
            } else if ((c & 0xE0) == 0xC0) {
                cp = c & 0x1F;
                need = 1;
                continue;
            } else if ((c & 0xF0) == 0xE0) {
                cp = c & 0x0F;
                need = 2;
                continue;
            } else if ((c & 0xF8) == 0xF0) {
                cp = c & 0x07;
                need = 3;
                continue;
            } else {
                continue;
            }
        } else {
            if ((c & 0xC0) != 0x80) {
                need = 0;
                continue;
            }
            cp = (cp << 6) | (c & 0x3F);
            if (--need) continue;
        }

        /* Treat CRLF as a single Enter. */
        if (cp == '\n' && last_cr) {
            last_cr = false;
            continue;
        }
        last_cr = (cp == '\r');

        uint8_t mods, key;
        if (keymap_lookup(cp, &mods, &key)) {
            tap(mods, key);
        } else {
            ESP_LOGD(TAG, "no mapping for U+%04lX", (unsigned long)cp);
        }
    }
}

bool kb_type(const char *utf8, size_t len)
{
    s_type_cancel = false;
    size_t sent = xStreamBufferSend(s_type_buf, utf8, len, pdMS_TO_TICKS(500));
    if (sent < len) ESP_LOGW(TAG, "type buffer full, dropped %u bytes", (unsigned)(len - sent));
    return sent == len;
}

void kb_type_cancel(void)
{
    s_type_cancel = true;
    xStreamBufferReset(s_type_buf);
    kb_set_source(KB_SRC_TYPE, 0, NULL);
}

size_t kb_type_pending(void)
{
    return xStreamBufferBytesAvailable(s_type_buf);
}

/* ---- BIOS spam ---- */

static void spam_task(void *arg)
{
    int idx = 0;
    for (;;) {
        int n = s_spam_n;
        if (n == 0 || esp_timer_get_time() >= s_spam_until) {
            if (n) ESP_LOGI(TAG, "BIOS spam finished");
            s_spam_n = 0;
            kb_set_source(KB_SRC_SPAM, 0, NULL);
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            idx = 0;
            continue;
        }
        /* Taps sent while the host is rebooting (USB not mounted) are dropped by
         * usb_kbd, which is fine: we just keep cycling until it comes back. */
        uint8_t keys[6] = {s_spam_keys[idx++ % n]};
        kb_set_source(KB_SRC_SPAM, 0, keys);
        vTaskDelay(pdMS_TO_TICKS(SPAM_PRESS_MS));
        kb_set_source(KB_SRC_SPAM, 0, NULL);
        vTaskDelay(pdMS_TO_TICKS(SPAM_GAP_MS));
    }
}

void kb_spam_start(const uint8_t *keys, int n, int seconds)
{
    if (n > KB_SPAM_MAX_KEYS) n = KB_SPAM_MAX_KEYS;
    s_spam_n = 0; /* pause the task while the key list changes */
    memcpy(s_spam_keys, keys, n);
    s_spam_until = esp_timer_get_time() + (int64_t)seconds * 1000000;
    s_spam_n = n;
    if (n) ESP_LOGI(TAG, "BIOS spam on: %d keys for %ds", n, seconds);
    xTaskNotifyGive(s_spam_task);
}

int kb_spam_remaining(void)
{
    if (!s_spam_n) return 0;
    int64_t left = s_spam_until - esp_timer_get_time();
    return left > 0 ? (int)((left + 999999) / 1000000) : 0;
}

void kb_init(void)
{
    s_mx = xSemaphoreCreateMutex();
    s_type_buf = xStreamBufferCreate(TYPE_BUF_SIZE, 1);
    xTaskCreate(type_task, "typer", 3072, NULL, 5, NULL);
    xTaskCreate(spam_task, "spam", 2560, NULL, 5, &s_spam_task);
}
