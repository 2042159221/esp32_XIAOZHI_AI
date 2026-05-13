#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t audio_diag_i2c_scan(void);
esp_err_t audio_diag_play_1khz_tone(void);
esp_err_t audio_diag_print_mic_rms(void);
esp_err_t audio_diag_loopback(void);

#ifdef __cplusplus
}
#endif
