/* Wi-Fi soft-AP + web UI. All interaction is over one WebSocket (/ws) so key
 * down/up events stay ordered. Text protocol, one command per frame:
 *   d<mods> <key>   key down        u<mods> <key>   key up
 *   t<text>         type UTF-8 text c               cancel typing
 *   r               release all     s               status (JSON reply)
 *   p               pair keyboard   f               forget all keyboards
 *   x               turn Wi-Fi off
 *   b<k> <k> ...    BIOS spam: cycle these key codes ("b" alone stops)
 *   F               reboot into USB download mode (for flashing) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "soc/rtc_cntl_reg.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "espkb.h"

static const char *TAG = "web";

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

#define MAX_FRAME 8192
#define SPAM_SECONDS 60

static httpd_handle_t s_httpd;
static volatile int s_ws_fd = -1;
static volatile int64_t s_last_activity;

static void touch(void)
{
    s_last_activity = esp_timer_get_time();
}

static esp_err_t index_handler(httpd_req_t *req)
{
    touch();
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

static esp_err_t send_status(httpd_req_t *req)
{
    ble_status_t st;
    ble_host_status(&st);

    char buf[512];
    int n = snprintf(buf, sizeof(buf), "{\"usb\":%d,\"sleep\":%d,\"pair\":%d,\"pk\":%d,\"q\":%u,\"spam\":%d,\"kb\":[",
                     usb_kbd_mounted(), usb_kbd_suspended(), st.pairing, st.passkey,
                     (unsigned)kb_type_pending(), kb_spam_remaining());
    for (int i = 0; i < st.count && n < sizeof(buf) - 64; i++) {
        n += snprintf(buf + n, sizeof(buf) - n, "%s{\"n\":\"%s\",\"r\":%d}", i ? "," : "", st.kb[i].name,
                      st.kb[i].ready);
    }
    n += snprintf(buf + n, sizeof(buf) - n, "]}");

    httpd_ws_frame_t f = {.type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)buf, .len = n};
    return httpd_ws_send_frame(req, &f);
}

static esp_err_t handle_msg(httpd_req_t *req, char *msg, size_t len)
{
    unsigned m = 0, k = 0;
    switch (msg[0]) {
    case 'd':
    case 'u':
        if (sscanf(msg + 1, "%u %u", &m, &k) == 2) kb_web_key(msg[0] == 'd', m & 0xFF, k & 0xFF);
        break;
    case 't':
        kb_type(msg + 1, len - 1);
        break;
    case 'c':
        kb_type_cancel();
        break;
    case 'r':
        kb_web_release();
        break;
    case 's':
        return send_status(req);
    case 'p':
        app_post(EV_PAIR);
        break;
    case 'f':
        ble_host_forget_all();
        break;
    case 'x':
        app_post(EV_WEB_OFF);
        break;
    case 'F':
        ESP_LOGW(TAG, "rebooting into download mode");
        kb_web_release();
        vTaskDelay(pdMS_TO_TICKS(200));
        REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
        esp_restart();
        break;
    case 'b': {
        uint8_t keys[KB_SPAM_MAX_KEYS];
        int n = 0;
        char *p = msg + 1, *end;
        while (n < KB_SPAM_MAX_KEYS) {
            unsigned long v = strtoul(p, &end, 10);
            if (end == p) break;
            if (v > 3 && v < 0xE0) keys[n++] = v; /* ordinary keys only, no modifiers */
            p = end;
        }
        kb_spam_start(keys, n, SPAM_SECONDS);
        break;
    }
    }
    return ESP_OK;
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    touch();
    if (req->method == HTTP_GET) {
        /* New page: drop anything an earlier connection left held down. */
        kb_web_release();
        s_ws_fd = httpd_req_to_sockfd(req);
        return ESP_OK;
    }

    httpd_ws_frame_t f = {0};
    esp_err_t err = httpd_ws_recv_frame(req, &f, 0);
    if (err != ESP_OK) return err;
    if (f.type != HTTPD_WS_TYPE_TEXT || f.len == 0) return ESP_OK;
    if (f.len > MAX_FRAME) return ESP_FAIL;

    char *buf = malloc(f.len + 1);
    if (!buf) return ESP_ERR_NO_MEM;
    f.payload = (uint8_t *)buf;
    err = httpd_ws_recv_frame(req, &f, f.len);
    if (err == ESP_OK) {
        buf[f.len] = 0;
        err = handle_msg(req, buf, f.len);
    }
    free(buf);
    return err;
}

static void on_close(httpd_handle_t hd, int fd)
{
    if (fd == s_ws_fd) {
        /* Phone went away mid-press: never leave a key stuck down. */
        kb_web_release();
        s_ws_fd = -1;
    }
    close(fd);
}

void web_init(void)
{
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
}

void web_start(void)
{
    if (s_httpd) return;

    wifi_config_t wc = {
        .ap = {
            .channel = CONFIG_ESPKB_AP_CHANNEL,
            .max_connection = 2,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strlcpy((char *)wc.ap.ssid, CONFIG_ESPKB_AP_SSID, sizeof(wc.ap.ssid));
    wc.ap.ssid_len = strlen(CONFIG_ESPKB_AP_SSID);
    strlcpy((char *)wc.ap.password, CONFIG_ESPKB_AP_PASSWORD, sizeof(wc.ap.password));
    if (strlen(CONFIG_ESPKB_AP_PASSWORD) < 8) wc.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.close_fn = on_close;
    hc.lru_purge_enable = true;
    hc.stack_size = 6144;
    if (httpd_start(&s_httpd, &hc) != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed");
        s_httpd = NULL;
        return;
    }
    static const httpd_uri_t index_uri = {.uri = "/", .method = HTTP_GET, .handler = index_handler};
    static const httpd_uri_t ws_uri = {
        .uri = "/ws", .method = HTTP_GET, .handler = ws_handler, .is_websocket = true};
    httpd_register_uri_handler(s_httpd, &index_uri);
    httpd_register_uri_handler(s_httpd, &ws_uri);

    touch();
    ESP_LOGI(TAG, "web UI on: join Wi-Fi '%s', open http://192.168.4.1", CONFIG_ESPKB_AP_SSID);
}

void web_stop(void)
{
    if (!s_httpd) return;
    httpd_stop(s_httpd);
    s_httpd = NULL;
    s_ws_fd = -1;
    esp_wifi_stop();
    kb_web_release();
    ESP_LOGI(TAG, "web UI off");
}

bool web_running(void)
{
    return s_httpd != NULL;
}

int64_t web_last_activity_us(void)
{
    return s_last_activity;
}
