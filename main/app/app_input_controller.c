#include "app_input_controller.h"

#include <stdint.h>
#include <stdbool.h>

#include "board_config.h"
#include "bsp_button.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "app_input";

static bool s_buttons_registered;
static app_input_controller_config_t s_config;

static void adc_button_cb(void *button_handle, void *usr_data);
static esp_err_t register_adc_button_events(int button_index);

esp_err_t app_input_controller_init(const app_input_controller_config_t *config)
{
    if (config != NULL) {
        s_config = *config;
    }

    if (s_buttons_registered) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(bsp_button_init(), TAG, "init ADC buttons failed");
    ESP_RETURN_ON_ERROR(register_adc_button_events(BOARD_BUTTON_SW2), TAG, "register SW2 button events failed");
    ESP_RETURN_ON_ERROR(register_adc_button_events(BOARD_BUTTON_SW3), TAG, "register SW3 button events failed");

    s_buttons_registered = true;
    ESP_LOGI(TAG, "ADC button events registered");
    return ESP_OK;
}

static esp_err_t register_adc_button_events(int button_index)
{
    ESP_RETURN_ON_ERROR(bsp_button_register_cb(button_index, BUTTON_PRESS_DOWN, adc_button_cb, (void *)(intptr_t)button_index), TAG, "register press down failed");
    ESP_RETURN_ON_ERROR(bsp_button_register_cb(button_index, BUTTON_PRESS_UP, adc_button_cb, (void *)(intptr_t)button_index), TAG, "register press up failed");
    ESP_RETURN_ON_ERROR(bsp_button_register_cb(button_index, BUTTON_SINGLE_CLICK, adc_button_cb, (void *)(intptr_t)button_index), TAG, "register single click failed");
    ESP_RETURN_ON_ERROR(bsp_button_register_cb(button_index, BUTTON_DOUBLE_CLICK, adc_button_cb, (void *)(intptr_t)button_index), TAG, "register double click failed");
    ESP_RETURN_ON_ERROR(bsp_button_register_cb(button_index, BUTTON_LONG_PRESS_START, adc_button_cb, (void *)(intptr_t)button_index), TAG, "register long press start failed");
    ESP_RETURN_ON_ERROR(bsp_button_register_cb(button_index, BUTTON_LONG_PRESS_HOLD, adc_button_cb, (void *)(intptr_t)button_index), TAG, "register long press hold failed");
    ESP_RETURN_ON_ERROR(bsp_button_register_cb(button_index, BUTTON_LONG_PRESS_UP, adc_button_cb, (void *)(intptr_t)button_index), TAG, "register long press up failed");

    return ESP_OK;
}

static void adc_button_cb(void *button_handle, void *usr_data)
{
    int button_index = (int)(intptr_t)usr_data;
    button_event_t event = iot_button_get_event(button_handle);

    ESP_LOGW(TAG, "adc button %d %s", button_index, iot_button_get_event_str(event));
    if (button_index == BOARD_BUTTON_SW2 && event == BUTTON_SINGLE_CLICK) {
        ESP_LOGW(TAG, "");
        ESP_LOGW(TAG, "========== SW2 RESET PROVISIONING ==========");
        ESP_LOGW(TAG, "SW2 single click detected");
        ESP_LOGW(TAG, "clearing saved WiFi/provisioning data now");
        ESP_LOGW(TAG, "device will restart and prefer configured provisioning");
        ESP_LOGW(TAG, "===========================================");

        if (s_config.reset_provisioning_cb != NULL) {
            esp_err_t err = s_config.reset_provisioning_cb(s_config.user_ctx);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "reset provisioning failed: %s", esp_err_to_name(err));
            }
        }
    }
}
