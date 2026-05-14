#include "audio_opus_diag.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "audio_opus_codec.h"
#include "bsp_audio.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "audio_opus_diag";

#ifndef CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_TONE_LOOPBACK_MS
#define CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_TONE_LOOPBACK_MS 5000
#endif

#ifndef CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_MIC_LOOPBACK_MS
#define CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_MIC_LOOPBACK_MS 10000
#endif

#ifndef CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_MIC_INPUT_BYTES
#define CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_MIC_INPUT_BYTES 1024
#endif

#ifndef CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_LOG_WINDOW_MS
#define CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_LOG_WINDOW_MS 500
#endif

#ifndef CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_VOLUME
#define CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_VOLUME 65
#endif

#ifndef CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_TONE_AMPLITUDE
#define CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_TONE_AMPLITUDE 10000
#endif

#define AUDIO_OPUS_1KHZ_PERIOD_SAMPLES 16
#define AUDIO_OPUS_RING_BYTES (AUDIO_OPUS_PCM_FRAME_BYTES * 4)

typedef struct {
    uint8_t *data;
    size_t capacity;
    size_t read_pos;
    size_t write_pos;
    size_t used;
} audio_opus_pcm_ring_t;

static const int16_t s_1khz_lut[AUDIO_OPUS_1KHZ_PERIOD_SAMPLES] = {
    0,
    3827,
    7071,
    9239,
    CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_TONE_AMPLITUDE,
    9239,
    7071,
    3827,
    0,
    -3827,
    -7071,
    -9239,
    -CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_TONE_AMPLITUDE,
    -9239,
    -7071,
    -3827,
};

static void *alloc_diag_buffer(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static void free_diag_buffer(void *buffer)
{
    heap_caps_free(buffer);
}

static esp_err_t open_audio_for_opus_diag(void)
{
    ESP_RETURN_ON_ERROR(bsp_audio_open(), TAG, "open audio codec failed");
    ESP_RETURN_ON_ERROR(bsp_audio_set_volume(CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_VOLUME), TAG, "set opus diag volume failed");
    ESP_RETURN_ON_FALSE(BSP_AUDIO_SAMPLE_RATE == AUDIO_OPUS_SAMPLE_RATE, ESP_ERR_NOT_SUPPORTED, TAG, "Opus diagnostics expect 16 kHz audio");
    ESP_RETURN_ON_FALSE(BSP_AUDIO_BITS_PER_SAMPLE == AUDIO_OPUS_BITS_PER_SAMPLE && BSP_AUDIO_CHANNELS == AUDIO_OPUS_CHANNELS,
                        ESP_ERR_NOT_SUPPORTED,
                        TAG,
                        "Opus diagnostics expect 16-bit mono PCM");
    return ESP_OK;
}

static void fill_1khz_opus_frame(uint8_t *pcm_frame, uint32_t frame_index)
{
    int16_t *samples = (int16_t *)pcm_frame;
    const size_t sample_count = AUDIO_OPUS_PCM_FRAME_BYTES / sizeof(int16_t);
    const uint32_t sample_base = frame_index * sample_count;

    for (size_t i = 0; i < sample_count; ++i) {
        samples[i] = s_1khz_lut[(sample_base + i) % AUDIO_OPUS_1KHZ_PERIOD_SAMPLES];
    }
}

static void ring_reset(audio_opus_pcm_ring_t *ring, uint8_t *storage, size_t capacity)
{
    ring->data = storage;
    ring->capacity = capacity;
    ring->read_pos = 0;
    ring->write_pos = 0;
    ring->used = 0;
}

static bool ring_write(audio_opus_pcm_ring_t *ring, const uint8_t *data, size_t len)
{
    if (ring->capacity - ring->used < len) {
        return false;
    }

    size_t first = ring->capacity - ring->write_pos;
    if (first > len) {
        first = len;
    }
    memcpy(ring->data + ring->write_pos, data, first);

    size_t remaining = len - first;
    if (remaining > 0) {
        memcpy(ring->data, data + first, remaining);
    }

    ring->write_pos = (ring->write_pos + len) % ring->capacity;
    ring->used += len;
    return true;
}

static bool ring_read(audio_opus_pcm_ring_t *ring, uint8_t *data, size_t len)
{
    if (ring->used < len) {
        return false;
    }

    size_t first = ring->capacity - ring->read_pos;
    if (first > len) {
        first = len;
    }
    memcpy(data, ring->data + ring->read_pos, first);

    size_t remaining = len - first;
    if (remaining > 0) {
        memcpy(data + first, ring->data, remaining);
    }

    ring->read_pos = (ring->read_pos + len) % ring->capacity;
    ring->used -= len;
    return true;
}

static bool should_log_frame(uint32_t frame_index, uint32_t log_window_frames)
{
    return frame_index < 3 || ((frame_index + 1) % log_window_frames) == 0;
}

static esp_err_t encode_decode_play(audio_opus_codec_t *codec,
                                    const uint8_t *pcm_frame,
                                    uint8_t *opus_frame,
                                    uint8_t *decoded_frame,
                                    uint32_t frame_index,
                                    uint32_t log_window_frames,
                                    const char *source)
{
    size_t opus_len = 0;
    size_t decoded_len = 0;

    ESP_RETURN_ON_ERROR(audio_opus_codec_encode(codec,
                                                pcm_frame,
                                                codec->pcm_frame_bytes,
                                                opus_frame,
                                                codec->opus_frame_bytes,
                                                &opus_len),
                        TAG,
                        "encode opus frame failed");
    ESP_RETURN_ON_ERROR(audio_opus_codec_decode(codec,
                                                opus_frame,
                                                opus_len,
                                                decoded_frame,
                                                codec->decoded_frame_bytes,
                                                &decoded_len),
                        TAG,
                        "decode opus frame failed");

    if (should_log_frame(frame_index, log_window_frames)) {
        ESP_LOGI(TAG,
                 "%s opus loopback frame=%u pcm input bytes=%u opus encoded bytes=%u decoded pcm bytes=%u free heap=%u minimum free heap=%u",
                 source,
                 (unsigned int)(frame_index + 1),
                 (unsigned int)codec->pcm_frame_bytes,
                 (unsigned int)opus_len,
                 (unsigned int)decoded_len,
                 (unsigned int)esp_get_free_heap_size(),
                 (unsigned int)esp_get_minimum_free_heap_size());
    }

    int write_ret = esp_codec_dev_write(bsp_audio_get_codec(), decoded_frame, (int)decoded_len);
    if (write_ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "%s opus loopback playback failed on frame %u: %d", source, (unsigned int)(frame_index + 1), write_ret);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t alloc_loopback_buffers(audio_opus_codec_t *codec,
                                        uint8_t **pcm_frame,
                                        uint8_t **opus_frame,
                                        uint8_t **decoded_frame)
{
    *pcm_frame = (uint8_t *)alloc_diag_buffer(codec->pcm_frame_bytes);
    *opus_frame = (uint8_t *)alloc_diag_buffer(codec->opus_frame_bytes);
    *decoded_frame = (uint8_t *)alloc_diag_buffer(codec->decoded_frame_bytes);

    if (*pcm_frame == NULL || *opus_frame == NULL || *decoded_frame == NULL) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static void free_loopback_buffers(uint8_t *pcm_frame, uint8_t *opus_frame, uint8_t *decoded_frame)
{
    free_diag_buffer(pcm_frame);
    free_diag_buffer(opus_frame);
    free_diag_buffer(decoded_frame);
}

esp_err_t audio_opus_diag_play_1khz_loopback(void)
{
    ESP_RETURN_ON_ERROR(open_audio_for_opus_diag(), TAG, "prepare audio codec failed");

    audio_opus_codec_t codec = {0};
    uint8_t *pcm_frame = NULL;
    uint8_t *opus_frame = NULL;
    uint8_t *decoded_frame = NULL;
    esp_err_t ret = audio_opus_codec_open(&codec);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = alloc_loopback_buffers(&codec, &pcm_frame, &opus_frame, &decoded_frame);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "alloc 1 kHz opus loopback buffers failed");
        goto cleanup;
    }

    uint32_t total_frames = (CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_TONE_LOOPBACK_MS + AUDIO_OPUS_FRAME_DURATION_MS - 1) / AUDIO_OPUS_FRAME_DURATION_MS;
    if (total_frames == 0) {
        total_frames = 1;
    }
    uint32_t log_window_frames = (CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_LOG_WINDOW_MS + AUDIO_OPUS_FRAME_DURATION_MS - 1) / AUDIO_OPUS_FRAME_DURATION_MS;
    if (log_window_frames == 0) {
        log_window_frames = 1;
    }

    int mute_ret = esp_codec_dev_set_out_mute(bsp_audio_get_codec(), false);
    if (mute_ret != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "unmute output before opus 1 kHz loopback failed: %d", mute_ret);
    }

    ESP_LOGI(TAG,
             "start 1 kHz PCM -> Opus encode -> Opus decode -> speaker test duration_ms=%u volume=%u pcm_frame_bytes=%u",
             (unsigned int)CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_TONE_LOOPBACK_MS,
             (unsigned int)CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_VOLUME,
             (unsigned int)codec.pcm_frame_bytes);

    for (uint32_t frame_index = 0; frame_index < total_frames; ++frame_index) {
        fill_1khz_opus_frame(pcm_frame, frame_index);
        ret = encode_decode_play(&codec, pcm_frame, opus_frame, decoded_frame, frame_index, log_window_frames, "tone");
        if (ret != ESP_OK) {
            goto cleanup;
        }
        vTaskDelay(pdMS_TO_TICKS(AUDIO_OPUS_FRAME_DURATION_MS));
    }

    memset(decoded_frame, 0, codec.decoded_frame_bytes);
    (void)esp_codec_dev_write(bsp_audio_get_codec(), decoded_frame, (int)codec.decoded_frame_bytes);
    ESP_LOGI(TAG, "1 kHz opus loopback finished");

cleanup:
    (void)esp_codec_dev_set_out_mute(bsp_audio_get_codec(), true);
    free_loopback_buffers(pcm_frame, opus_frame, decoded_frame);
    audio_opus_codec_close(&codec);
    return ret;
}

esp_err_t audio_opus_diag_mic_loopback(void)
{
    ESP_RETURN_ON_ERROR(open_audio_for_opus_diag(), TAG, "prepare audio codec failed");
    ESP_RETURN_ON_FALSE((CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_MIC_INPUT_BYTES % sizeof(int16_t)) == 0,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "mic input bytes must be aligned to 16-bit samples");
    ESP_RETURN_ON_FALSE(CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_MIC_INPUT_BYTES > 0 &&
                            CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_MIC_INPUT_BYTES <= AUDIO_OPUS_RING_BYTES,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "invalid mic input bytes");

    audio_opus_codec_t codec = {0};
    audio_opus_pcm_ring_t ring = {0};
    uint8_t *pcm_frame = NULL;
    uint8_t *opus_frame = NULL;
    uint8_t *decoded_frame = NULL;
    uint8_t *mic_chunk = NULL;
    uint8_t *ring_storage = NULL;
    esp_err_t ret = audio_opus_codec_open(&codec);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = alloc_loopback_buffers(&codec, &pcm_frame, &opus_frame, &decoded_frame);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "alloc mic opus loopback buffers failed");
        goto cleanup;
    }

    mic_chunk = (uint8_t *)alloc_diag_buffer(CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_MIC_INPUT_BYTES);
    ring_storage = (uint8_t *)alloc_diag_buffer(AUDIO_OPUS_RING_BYTES);
    if (mic_chunk == NULL || ring_storage == NULL) {
        ESP_LOGE(TAG, "alloc mic ring buffers failed");
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    ring_reset(&ring, ring_storage, AUDIO_OPUS_RING_BYTES);

    uint32_t total_ms = 0;
    uint32_t frame_index = 0;
    uint32_t log_window_frames = (CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_LOG_WINDOW_MS + AUDIO_OPUS_FRAME_DURATION_MS - 1) / AUDIO_OPUS_FRAME_DURATION_MS;
    if (log_window_frames == 0) {
        log_window_frames = 1;
    }

    int mute_ret = esp_codec_dev_set_out_mute(bsp_audio_get_codec(), false);
    if (mute_ret != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "unmute output before opus mic loopback failed: %d", mute_ret);
    }

    ESP_LOGI(TAG,
             "start mic PCM -> ring buffer -> Opus encode -> Opus decode -> speaker test duration_ms=%u mic_input_bytes=%u opus_pcm_frame_bytes=%u volume=%u",
             (unsigned int)CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_MIC_LOOPBACK_MS,
             (unsigned int)CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_MIC_INPUT_BYTES,
             (unsigned int)codec.pcm_frame_bytes,
             (unsigned int)CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_VOLUME);

    while (total_ms < CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_MIC_LOOPBACK_MS) {
        int read_ret = esp_codec_dev_read(bsp_audio_get_codec(), mic_chunk, CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_MIC_INPUT_BYTES);
        if (read_ret != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "mic opus loopback capture failed: %d", read_ret);
            ret = ESP_FAIL;
            goto cleanup;
        }

        if (should_log_frame(frame_index, log_window_frames)) {
            ESP_LOGI(TAG,
                     "mic ring input bytes=%u ring_used=%u free heap=%u minimum free heap=%u",
                     (unsigned int)CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_MIC_INPUT_BYTES,
                     (unsigned int)ring.used,
                     (unsigned int)esp_get_free_heap_size(),
                     (unsigned int)esp_get_minimum_free_heap_size());
        }

        if (!ring_write(&ring, mic_chunk, CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_MIC_INPUT_BYTES)) {
            ESP_LOGE(TAG, "mic opus ring buffer overflow used=%u write=%u capacity=%u",
                     (unsigned int)ring.used,
                     (unsigned int)CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_MIC_INPUT_BYTES,
                     (unsigned int)ring.capacity);
            ret = ESP_ERR_NO_MEM;
            goto cleanup;
        }

        while (ring.used >= codec.pcm_frame_bytes && total_ms < CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_MIC_LOOPBACK_MS) {
            if (!ring_read(&ring, pcm_frame, codec.pcm_frame_bytes)) {
                ESP_LOGE(TAG, "mic opus ring buffer read failed");
                ret = ESP_FAIL;
                goto cleanup;
            }

            ret = encode_decode_play(&codec, pcm_frame, opus_frame, decoded_frame, frame_index, log_window_frames, "mic");
            if (ret != ESP_OK) {
                goto cleanup;
            }

            frame_index++;
            total_ms += AUDIO_OPUS_FRAME_DURATION_MS;
        }
    }

    memset(decoded_frame, 0, codec.decoded_frame_bytes);
    (void)esp_codec_dev_write(bsp_audio_get_codec(), decoded_frame, (int)codec.decoded_frame_bytes);
    ESP_LOGI(TAG, "mic opus loopback finished frames=%u", (unsigned int)frame_index);

cleanup:
    (void)esp_codec_dev_set_out_mute(bsp_audio_get_codec(), true);
    free_diag_buffer(mic_chunk);
    free_diag_buffer(ring_storage);
    free_loopback_buffers(pcm_frame, opus_frame, decoded_frame);
    audio_opus_codec_close(&codec);
    return ret;
}
