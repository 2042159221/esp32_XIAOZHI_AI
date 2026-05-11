#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t xiaozhi_ui_init(void);
void xiaozhi_ui_show_qrcode(const char *payload);
void xiaozhi_ui_show_title(const char *title);
void xiaozhi_ui_show_text(const char *text);
void xiaozhi_ui_show_emoji(const char *emoji_name);
void xiaozhi_ui_show_activation_required(const char *activation_code);
void xiaozhi_ui_show_welcome(void);
void xiaozhi_ui_show_error(const char *title, const char *message);
void xiaozhi_ui_show_ota_loading(void);

#ifdef __cplusplus
}
#endif
