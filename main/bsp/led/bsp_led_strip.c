#include "bsp_led_strip.h"

#include <stdbool.h>

#include "board_config.h"
#include "esp_check.h"
#include "esp_log.h"
#include "led_strip.h"

static const char *TAG = "bsp_led_strip";

static led_strip_handle_t s_strip;

esp_err_t bsp_led_strip_init(void)
{
    if (s_strip != NULL) {
        return ESP_OK;
    }

    led_strip_config_t strip_config = {
        .strip_gpio_num = BOARD_STATUS_LED_GPIO,
        .max_leds = BOARD_STATUS_LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .flags = {
            .invert_out = false,
        },
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 64,
        .flags = {
            .with_dma = false,
        },
    };

    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip), TAG, "init led strip failed");
    ESP_RETURN_ON_ERROR(led_strip_clear(s_strip), TAG, "clear led strip failed");
    ESP_LOGI(TAG, "status led strip initialized on GPIO%d", BOARD_STATUS_LED_GPIO);
    return ESP_OK;
}

esp_err_t bsp_led_strip_set_pixel(uint32_t index, uint8_t red, uint8_t green, uint8_t blue)
{
    ESP_RETURN_ON_FALSE(s_strip != NULL, ESP_ERR_INVALID_STATE, TAG, "led strip is not initialized");
    return led_strip_set_pixel(s_strip, index, red, green, blue);
}

esp_err_t bsp_led_strip_refresh(void)
{
    ESP_RETURN_ON_FALSE(s_strip != NULL, ESP_ERR_INVALID_STATE, TAG, "led strip is not initialized");
    return led_strip_refresh(s_strip);
}

esp_err_t bsp_led_strip_clear(void)
{
    ESP_RETURN_ON_FALSE(s_strip != NULL, ESP_ERR_INVALID_STATE, TAG, "led strip is not initialized");
    return led_strip_clear(s_strip);
}
