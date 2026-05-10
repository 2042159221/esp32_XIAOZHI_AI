#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t app_display_init(void);
bool app_display_is_initialized(void);
void app_display_show_ble_qrcode(const char *payload);
void app_display_show_message(const char *title, const char *message);
void app_display_show_color_bars_test(void);

#ifdef __cplusplus
}
#endif
