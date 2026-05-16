#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t app_controller_start(void);
esp_err_t app_controller_start_voice_session(void);

#ifdef __cplusplus
}
#endif
