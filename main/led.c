#include "driver/gpio.h"
#include "espkb.h"

#if CONFIG_ESPKB_LED_WS2812
#include "led_strip.h"
static led_strip_handle_t s_strip;
#endif

static uint32_t s_cur = 0xFFFFFFFF;

void led_init(void)
{
#if CONFIG_ESPKB_LED_WS2812
    led_strip_config_t sc = {
        .strip_gpio_num = CONFIG_ESPKB_LED_GPIO,
        .max_leds = 1,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
    };
    led_strip_rmt_config_t rc = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&sc, &rc, &s_strip));
#elif CONFIG_ESPKB_LED_GPIO_PLAIN
    gpio_reset_pin(CONFIG_ESPKB_LED_GPIO);
    gpio_set_direction(CONFIG_ESPKB_LED_GPIO, GPIO_MODE_OUTPUT);
#endif
    led_set(0, 0, 0);
}

void led_set(uint8_t r, uint8_t g, uint8_t b)
{
    uint32_t v = (r << 16) | (g << 8) | b;
    if (v == s_cur) return;
    s_cur = v;
#if CONFIG_ESPKB_LED_WS2812
    const int br = CONFIG_ESPKB_LED_BRIGHTNESS;
    led_strip_set_pixel(s_strip, 0, r * br / 255, g * br / 255, b * br / 255);
    led_strip_refresh(s_strip);
#elif CONFIG_ESPKB_LED_GPIO_PLAIN
    gpio_set_level(CONFIG_ESPKB_LED_GPIO, v != 0);
#endif
}
