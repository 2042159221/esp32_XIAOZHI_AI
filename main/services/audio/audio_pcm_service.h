#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef esp_err_t (*audio_pcm_tx_cb_t)(const uint8_t *data, size_t len, void *user_ctx);

esp_err_t audio_pcm_service_init(void);
esp_err_t audio_pcm_service_start_stream(audio_pcm_tx_cb_t tx_cb, void *user_ctx);
esp_err_t audio_pcm_service_stop_stream(void);
esp_err_t audio_pcm_service_enqueue_playback(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
