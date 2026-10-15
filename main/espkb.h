#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "sdkconfig.h"

/* ---- app events (main.c) ---- */
typedef enum {
    EV_BTN_TAP,
    EV_BTN_LONG,
    EV_WEB_OFF,
    EV_PAIR,
    EV_KB_CONNECTED,
} app_ev_t;

void app_post(app_ev_t ev);

/* ---- USB HID boot keyboard (usb_kbd.c) ---- */
void usb_kbd_init(void);
void usb_kbd_send(const uint8_t report[8]);
bool usb_kbd_mounted(void);
bool usb_kbd_suspended(void);

/* ---- report merging + text typing (keyboard.c) ----
 * Every input (each BLE keyboard, web keys, web typing) is a "source" with its
 * own modifier byte and 6 keys. The USB report is the union of all sources. */
#define KB_SRC_WEB  (CONFIG_ESPKB_MAX_KEYBOARDS)
#define KB_SRC_TYPE (CONFIG_ESPKB_MAX_KEYBOARDS + 1)
#define KB_NSRC     (CONFIG_ESPKB_MAX_KEYBOARDS + 2)

void kb_init(void);
void kb_set_source(int src, uint8_t mods, const uint8_t keys[6]);
void kb_web_key(bool down, uint8_t mods, uint8_t key);
void kb_web_release(void);
bool kb_type(const char *utf8, size_t len);
void kb_type_cancel(void);
size_t kb_type_pending(void);

/* ---- character -> HID usage (keymap.c) ---- */
#define KM_SHIFT 0x02
bool keymap_lookup(uint32_t codepoint, uint8_t *mods, uint8_t *key);

/* ---- BLE HID host (ble_host.c) ---- */
typedef struct {
    int count;
    struct {
        char name[32];
        bool ready;
    } kb[CONFIG_ESPKB_MAX_KEYBOARDS];
    bool pairing;
    int passkey; /* -1 when none */
} ble_status_t;

void ble_host_init(void);
void ble_host_start_pairing(int seconds);
bool ble_host_pairing(void);
void ble_host_forget_all(void);
void ble_host_set_leds(uint8_t leds);
void ble_host_status(ble_status_t *out);

/* ---- Wi-Fi AP + web UI (web.c) ---- */
void web_init(void);
void web_start(void);
void web_stop(void);
bool web_running(void);
int64_t web_last_activity_us(void);

/* ---- status LED (led.c) ---- */
void led_init(void);
void led_set(uint8_t r, uint8_t g, uint8_t b);
