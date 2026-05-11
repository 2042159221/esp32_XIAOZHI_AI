#include "bsp_button.h"

#include <stddef.h>

#include "board_config.h"
#include "button_adc.h"
#include "esp_check.h"

static const char *TAG = "bsp_button";

enum {
    BSP_BUTTON_HANDLE_COUNT = BOARD_BUTTON_MAX_INDEX + 1,
};

static button_handle_t s_adc_buttons[BSP_BUTTON_HANDLE_COUNT];

static const button_adc_config_t s_adc_button_configs[] = {
    {
        .unit_id = BOARD_BUTTON_ADC_UNIT,
        .adc_channel = BOARD_BUTTON_ADC_CHANNEL,
        .button_index = BOARD_BUTTON_SW2,
        .min = BOARD_BUTTON_SW2_MIN_MV,
        .max = BOARD_BUTTON_SW2_MAX_MV,
    },
    {
        .unit_id = BOARD_BUTTON_ADC_UNIT,
        .adc_channel = BOARD_BUTTON_ADC_CHANNEL,
        .button_index = BOARD_BUTTON_SW3,
        .min = BOARD_BUTTON_SW3_MIN_MV,
        .max = BOARD_BUTTON_SW3_MAX_MV,
    },
};

esp_err_t bsp_button_init(void)
{
    const button_config_t button_config = {0};

    for (size_t i = 0; i < sizeof(s_adc_button_configs) / sizeof(s_adc_button_configs[0]); i++) {
        const button_adc_config_t *adc_config = &s_adc_button_configs[i];
        if (s_adc_buttons[adc_config->button_index] != NULL) {
            continue;
        }

        ESP_RETURN_ON_ERROR(iot_button_new_adc_device(&button_config,
                                                      adc_config,
                                                      &s_adc_buttons[adc_config->button_index]),
                            TAG,
                            "create adc button %d failed",
                            adc_config->button_index);
    }

    return ESP_OK;
}

esp_err_t bsp_button_register_cb(int button_index, button_event_t event, button_cb_t cb, void *usr_data)
{
    ESP_RETURN_ON_FALSE(button_index > 0 && button_index < BSP_BUTTON_HANDLE_COUNT,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "invalid button index %d",
                        button_index);
    ESP_RETURN_ON_FALSE(s_adc_buttons[button_index] != NULL,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "button %d is not initialized",
                        button_index);

    return iot_button_register_cb(s_adc_buttons[button_index], event, NULL, cb, usr_data);
}

esp_err_t bsp_button_sw2_register_cb(button_event_t event, button_cb_t cb, void *usr_data)
{
    return bsp_button_register_cb(BOARD_BUTTON_SW2, event, cb, usr_data);
}

esp_err_t bsp_button_sw3_register_cb(button_event_t event, button_cb_t cb, void *usr_data)
{
    return bsp_button_register_cb(BOARD_BUTTON_SW3, event, cb, usr_data);
}
