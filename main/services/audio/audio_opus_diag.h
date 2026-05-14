#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t audio_opus_diag_play_1khz_loopback(void);
esp_err_t audio_opus_diag_mic_loopback(void);

#ifdef __cplusplus
}
#endif
