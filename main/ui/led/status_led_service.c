#include "status_led_service.h"

#include <stdbool.h>
#include <math.h>
#include <stdint.h>

#include "board_config.h"
#include "bsp_led_strip.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "status_led";

static TaskHandle_t s_led_task;

static void status_led_task(void *arg);
static void hsv_to_rgb(uint16_t hue, uint8_t saturation, uint8_t value, uint8_t *red, uint8_t *green, uint8_t *blue);

esp_err_t status_led_service_start(void)
{
    if (s_led_task != NULL) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(bsp_led_strip_init(), TAG, "init status led strip failed");

    BaseType_t created = xTaskCreate(status_led_task,
                                     "status_led",
                                     BOARD_STATUS_LED_TASK_STACK,
                                     NULL,
                                     BOARD_STATUS_LED_TASK_PRIORITY,
                                     &s_led_task);
    ESP_RETURN_ON_FALSE(created == pdPASS, ESP_ERR_NO_MEM, TAG, "create led task failed");
    ESP_LOGI(TAG, "status led animation started");
    return ESP_OK;
}

static void status_led_task(void *arg)
{
    (void)arg;

    uint16_t frame = 0;
    while (true) {
        uint8_t pulse = (uint8_t)(32 + ((sin(frame * 0.12f) + 1.0f) * 72.0f));
        for (uint32_t led = 0; led < BOARD_STATUS_LED_COUNT; led++) {
            uint8_t red;
            uint8_t green;
            uint8_t blue;
            uint16_t hue = (uint16_t)((frame * 7 + led * 180) % 360);
            uint8_t sparkle = ((frame + led * 11) % 29 == 0) ? 140 : pulse;
            hsv_to_rgb(hue, 255, sparkle, &red, &green, &blue);
            ESP_ERROR_CHECK_WITHOUT_ABORT(bsp_led_strip_set_pixel(led, red, green, blue));
        }
        ESP_ERROR_CHECK_WITHOUT_ABORT(bsp_led_strip_refresh());

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
