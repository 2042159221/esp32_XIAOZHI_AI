#pragma once

#include "esp_err.h"
#include "iot_button.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t bsp_button_init(void);
esp_err_t bsp_button_register_cb(int button_index, button_event_t event, button_cb_t cb, void *usr_data);
esp_err_t bsp_button_sw2_register_cb(button_event_t event, button_cb_t cb, void *usr_data);
esp_err_t bsp_button_sw3_register_cb(button_event_t event, button_cb_t cb, void *usr_data);

#ifdef __cplusplus
}
#endif
