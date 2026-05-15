#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef esp_err_t (*audio_opus_stream_send_cb_t)(const uint8_t *opus, size_t len, void *user_ctx);

typedef struct {
    audio_opus_stream_send_cb_t send_cb;
    void *user_ctx;
    int output_volume;
} audio_opus_stream_config_t;

esp_err_t audio_opus_stream_start(const audio_opus_stream_config_t *config);
esp_err_t audio_opus_stream_stop(void);
esp_err_t audio_opus_stream_set_uplink_enabled(bool enabled);
bool audio_opus_stream_is_uplink_enabled(void);
esp_err_t audio_opus_stream_feed_pcm(const uint8_t *pcm, size_t len);
esp_err_t audio_opus_stream_enqueue_downlink_opus(const uint8_t *opus, size_t len);
void audio_opus_stream_flush(void);

#ifdef __cplusplus
}
#endif
