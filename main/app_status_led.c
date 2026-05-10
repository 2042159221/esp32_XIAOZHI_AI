#include "app_status_led.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

#define APP_STATUS_LED_GPIO GPIO_NUM_46
#define APP_STATUS_LED_COUNT 2
#define APP_STATUS_LED_TASK_STACK 3072
#define APP_STATUS_LED_TASK_PRIORITY 4

static const char *TAG = "status_led";

static led_strip_handle_t s_strip;
static TaskHandle_t s_led_task;

static void status_led_task(void *arg);
static void hsv_to_rgb(uint16_t hue, uint8_t saturation, uint8_t value, uint8_t *red, uint8_t *green, uint8_t *blue);

esp_err_t app_status_led_start(void)
{
    if (s_led_task != NULL) {
        return ESP_OK;
    }

    if (s_strip == NULL) {
        led_strip_config_t strip_config = {
            .strip_gpio_num = APP_STATUS_LED_GPIO,
            .max_leds = APP_STATUS_LED_COUNT,
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
    }

    BaseType_t created = xTaskCreate(status_led_task,
                                     "status_led",
                                     APP_STATUS_LED_TASK_STACK,
                                     NULL,
                                     APP_STATUS_LED_TASK_PRIORITY,
                                     &s_led_task);
    ESP_RETURN_ON_FALSE(created == pdPASS, ESP_ERR_NO_MEM, TAG, "create led task failed");
    ESP_LOGI(TAG, "status led animation started on GPIO%d", APP_STATUS_LED_GPIO);
    return ESP_OK;
}

static void status_led_task(void *arg)
{
    (void)arg;

    uint16_t frame = 0;
    while (true) {
        uint8_t pulse = (uint8_t)(32 + ((sin(frame * 0.12f) + 1.0f) * 72.0f));
        for (uint32_t led = 0; led < APP_STATUS_LED_COUNT; led++) {
            uint8_t red;
            uint8_t green;
            uint8_t blue;
            uint16_t hue = (uint16_t)((frame * 7 + led * 180) % 360);
            uint8_t sparkle = ((frame + led * 11) % 29 == 0) ? 140 : pulse;
            hsv_to_rgb(hue, 255, sparkle, &red, &green, &blue);
            ESP_ERROR_CHECK_WITHOUT_ABORT(led_strip_set_pixel(s_strip, led, red, green, blue));
        }
        ESP_ERROR_CHECK_WITHOUT_ABORT(led_strip_refresh(s_strip));

        frame++;
        vTaskDelay(pdMS_TO_TICKS(35));
    }
}

static void hsv_to_rgb(uint16_t hue, uint8_t saturation, uint8_t value, uint8_t *red, uint8_t *green, uint8_t *blue)
{
    uint8_t region = hue / 60;
    uint16_t remainder = (hue - (region * 60)) * 255 / 60;

    uint8_t p = (uint8_t)((value * (255 - saturation)) / 255);
    uint8_t q = (uint8_t)((value * (255 - ((saturation * remainder) / 255))) / 255);
    uint8_t t = (uint8_t)((value * (255 - ((saturation * (255 - remainder)) / 255))) / 255);

    switch (region) {
    case 0:
        *red = value;
        *green = t;
        *blue = p;
        break;
    case 1:
        *red = q;
        *green = value;
        *blue = p;
        break;
    case 2:
        *red = p;
        *green = value;
        *blue = t;
        break;
    case 3:
        *red = p;
        *green = q;
        *blue = value;
        break;
    case 4:
        *red = t;
        *green = p;
        *blue = value;
        break;
    default:
        *red = value;
        *green = p;
        *blue = q;
        break;
    }
}
