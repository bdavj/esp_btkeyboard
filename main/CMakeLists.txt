idf_component_register(
    SRCS "main.c" "usb_kbd.c" "keyboard.c" "keymap.c" "ble_host.c" "web.c" "led.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "web/index.html"
    PRIV_REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_http_server esp_timer
                  esp_driver_gpio bt espcoredump
)
