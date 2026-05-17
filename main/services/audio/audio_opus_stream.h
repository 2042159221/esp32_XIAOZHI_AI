#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef esp_err_t (*audio_opus_stream_send_cb_t)(const uint8_t *opus, size_t len, void *user_ctx);

typedef enum {
    AUDIO_OPUS_PCM_SOURCE_NONE = 0,
    AUDIO_OPUS_PCM_SOURCE_DIRECT_CODEC,
    AUDIO_OPUS_PCM_SOURCE_EXTERNAL_FEED,
} audio_opus_pcm_source_t;

typedef struct {
    audio_opus_stream_send_cb_t send_cb;
    void *user_ctx;
    int output_volume;
    audio_opus_pcm_source_t pcm_source;
    int decoder_output_sample_rate;
} audio_opus_stream_config_t;

typedef struct {
    uint32_t tx_frames;
    uint32_t tx_bytes;
    uint32_t capture_frames;
    uint32_t uplink_drop_count;
} audio_opus_stream_stats_t;

esp_err_t audio_opus_stream_start(const audio_opus_stream_config_t *config);
esp_err_t audio_opus_stream_stop(void);
esp_err_t audio_opus_stream_set_uplink_enabled(bool enabled);
bool audio_opus_stream_is_uplink_enabled(void);
void audio_opus_stream_get_stats(audio_opus_stream_stats_t *out_stats);
esp_err_t audio_opus_stream_close_decoder(void);
esp_err_t audio_opus_stream_wait_downlink_idle(uint32_t timeout_ms);
esp_err_t audio_opus_stream_feed_pcm(const uint8_t *pcm, size_t len);
esp_err_t audio_opus_stream_enqueue_downlink_opus(const uint8_t *opus, size_t len);
void audio_opus_stream_flush(void);
void audio_opus_stream_log_watermarks(const char *label);

#ifdef __cplusplus
}
#endif
