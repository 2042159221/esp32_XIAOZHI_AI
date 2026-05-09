#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t app_display_init(void);
void app_display_show_ble_qrcode(const char *payload);
void app_display_show_message(const char *title, const char *message);
void app_display_show_color_bars_test(void);

#ifdef __cplusplus
}
#endif
