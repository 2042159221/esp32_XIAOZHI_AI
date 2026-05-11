#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t display_service_init(void);
bool display_service_is_initialized(void);
void display_service_show_qrcode(const char *payload);
void display_service_show_message(const char *title, const char *message);
void display_service_show_color_bars_test(void);

#ifdef __cplusplus
}
#endif
