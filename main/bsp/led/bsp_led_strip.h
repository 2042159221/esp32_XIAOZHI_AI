#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t bsp_led_strip_init(void);
esp_err_t bsp_led_strip_set_pixel(uint32_t index, uint8_t red, uint8_t green, uint8_t blue);
esp_err_t bsp_led_strip_refresh(void);
esp_err_t bsp_led_strip_clear(void);

#ifdef __cplusplus
}
#endif
