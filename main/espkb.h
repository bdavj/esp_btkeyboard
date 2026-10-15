#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "sdkconfig.h"

/* ---- app events (main.c) ---- */
typedef enum {
    EV_BTN_TAP,
    EV_BTN_LONG,
    EV_WEB_ON,
    EV_WEB_OFF,
    EV_PAIR,
    EV_KB_CONNECTED,
} app_ev_t;

void app_post(app_ev_t ev);
const char *app_boot_report(void);

/* ---- USB HID boot keyboard (usb_kbd.c) ---- */
void usb_kbd_init(void);
void usb_kbd_send(const uint8_t report[8]);
bool usb_kbd_mounted(void);
bool usb_kbd_suspended(void);
void usb_kbd_reboot_to_download(void);

/* ---- report merging + text typing (keyboard.c) ----
 * Every input (each BLE keyboard, web keys, web typing) is a "source" with its
 * own modifier byte and 6 keys. The USB report is the union of all sources. */
#define KB_SRC_WEB  (CONFIG_ESPKB_MAX_KEYBOARDS)
#define KB_SRC_TYPE (CONFIG_ESPKB_MAX_KEYBOARDS + 1)
#define KB_SRC_SPAM (CONFIG_ESPKB_MAX_KEYBOARDS + 2)
#define KB_NSRC     (CONFIG_ESPKB_MAX_KEYBOARDS + 3)

void kb_init(void);
void kb_set_source(int src, uint8_t mods, const uint8_t keys[6]);
void kb_web_key(bool down, uint8_t mods, uint8_t key);
void kb_web_release(void);
bool kb_type(const char *utf8, size_t len);
void kb_type_cancel(void);
size_t kb_type_pending(void);

/* "BIOS spam": repeatedly tap keys (e.g. Del/Esc/F2/F12) so the host's
 * firmware catches one during boot. n == 0 stops. Runs on the device, so it
 * keeps going while the phone sleeps or the host reboots. */
#define KB_SPAM_MAX_KEYS 8
void kb_spam_start(const uint8_t *keys, int n, int seconds);
int kb_spam_remaining(void); /* seconds left, 0 when off */

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
/* Wi-Fi and BLE share one radio: scan much less while the AP is up so phones
 * can complete the WPA2 handshake. */
void ble_host_set_wifi_active(bool on);
void ble_host_status(ble_status_t *out);
int ble_host_debug(char *buf, size_t size); /* multi-line text for the serial status */

/* ---- Wi-Fi AP + web UI (web.c) ---- */
void web_init(void);
void web_start(void);
void web_stop(void);
bool web_running(void);
int64_t web_last_activity_us(void);
int web_sta_count(void);
int web_join_count(void);
int web_leave_count(void);
int web_last_leave_reason(void);

/* ---- status LED (led.c) ---- */
void led_init(void);
void led_set(uint8_t r, uint8_t g, uint8_t b);
