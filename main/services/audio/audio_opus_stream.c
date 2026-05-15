#include "audio_opus_stream.h"

#include <stdbool.h>
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
#include "freertos/idf_additions.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"

static const char *TAG = "audio_opus_stream";

#ifndef CONFIG_XIAOZHI_AUDIO_OPUS_STREAM_VOLUME
#define CONFIG_XIAOZHI_AUDIO_OPUS_STREAM_VOLUME 65
#endif

#ifndef CONFIG_XIAOZHI_AUDIO_OPUS_STREAM_PCM_RING_BYTES
#define CONFIG_XIAOZHI_AUDIO_OPUS_STREAM_PCM_RING_BYTES 32768
#endif

#ifndef CONFIG_XIAOZHI_AUDIO_OPUS_STREAM_OPUS_RING_BYTES
#define CONFIG_XIAOZHI_AUDIO_OPUS_STREAM_OPUS_RING_BYTES 32768
#endif

#ifndef CONFIG_XIAOZHI_AUDIO_OPUS_STREAM_ENCODER_TASK_STACK_SIZE
#define CONFIG_XIAOZHI_AUDIO_OPUS_STREAM_ENCODER_TASK_STACK_SIZE 49152
#endif

#ifndef CONFIG_XIAOZHI_AUDIO_OPUS_STREAM_DECODER_TASK_STACK_SIZE
#define CONFIG_XIAOZHI_AUDIO_OPUS_STREAM_DECODER_TASK_STACK_SIZE 24576
#endif

#ifndef CONFIG_XIAOZHI_AUDIO_OPUS_STREAM_LOG_WINDOW_MS
#define CONFIG_XIAOZHI_AUDIO_OPUS_STREAM_LOG_WINDOW_MS 500
#endif

#define AUDIO_OPUS_STREAM_ACCUM_BYTES (AUDIO_OPUS_PCM_FRAME_BYTES * 4)
#define AUDIO_OPUS_STREAM_RECV_TIMEOUT_MS 100
#define AUDIO_OPUS_STREAM_SEND_TIMEOUT_MS 0
#define AUDIO_OPUS_STREAM_SHUTDOWN_WAIT_MS 1500
#define AUDIO_OPUS_STREAM_ENCODER_TASK_PRIORITY 6
#define AUDIO_OPUS_STREAM_DECODER_TASK_PRIORITY 5

typedef struct {
    uint8_t *data;
    size_t capacity;
    size_t read_pos;
    size_t write_pos;
    size_t used;
} pcm_accum_t;

typedef struct {
    RingbufHandle_t pcm_rb;
    RingbufHandle_t downlink_rb;
    TaskHandle_t encoder_task;
    TaskHandle_t decoder_task;
    audio_opus_codec_t codec;
    audio_opus_stream_send_cb_t send_cb;
    void *user_ctx;
    volatile bool running;
    volatile bool uplink_enabled;
    uint32_t tx_frames;
    uint32_t tx_bytes;
    uint32_t rx_frames;
    uint32_t decoded_frames;
    uint32_t playback_failures;
    uint32_t uplink_drop_count;
    uint32_t downlink_drop_count;
    uint32_t log_window_frames;
} audio_opus_stream_ctx_t;

static audio_opus_stream_ctx_t s_stream;

static void encoder_task(void *arg);
static void decoder_task(void *arg);

static RingbufHandle_t create_stream_ringbuffer(size_t size)
{
#if CONFIG_SPIRAM
    RingbufHandle_t ringbuffer = xRingbufferCreateWithCaps(size, RINGBUF_TYPE_NOSPLIT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ringbuffer != NULL) {
        return ringbuffer;
    }
    ESP_LOGW(TAG, "create PSRAM ringbuffer failed, fallback internal bytes=%u", (unsigned int)size);
#endif
    return xRingbufferCreateWithCaps(size, RINGBUF_TYPE_NOSPLIT, MALLOC_CAP_8BIT);
}

static void *alloc_stream_buffer(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static void delete_current_stream_task(void)
{
#if CONFIG_SPIRAM
    vTaskDeleteWithCaps(NULL);
#else
    vTaskDelete(NULL);
#endif
}

static esp_err_t create_stream_task(TaskFunction_t task_fn, const char *name, uint32_t stack_size, UBaseType_t priority, TaskHandle_t *handle)
{
#if CONFIG_SPIRAM
    BaseType_t created = xTaskCreatePinnedToCoreWithCaps(task_fn,
                                                         name,
                                                         stack_size,
                                                         NULL,
                                                         priority,
                                                         handle,
                                                         tskNO_AFFINITY,
                                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    BaseType_t created = xTaskCreate(task_fn, name, stack_size, NULL, priority, handle);
#endif
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

static void drain_ringbuffer(RingbufHandle_t rb)
{
    if (rb == NULL) {
        return;
    }

    while (true) {
        size_t item_size = 0;
        void *item = xRingbufferReceive(rb, &item_size, 0);
        if (item == NULL) {
            break;
        }
        vRingbufferReturnItem(rb, item);
    }
}

static bool should_log_frame(uint32_t frame_index)
{
    return frame_index <= 3 || (s_stream.log_window_frames > 0 && (frame_index % s_stream.log_window_frames) == 0);
}

static void accum_reset(pcm_accum_t *accum, uint8_t *storage, size_t capacity)
{
    accum->data = storage;
    accum->capacity = capacity;
    accum->read_pos = 0;
    accum->write_pos = 0;
    accum->used = 0;
}

static bool accum_write(pcm_accum_t *accum, const uint8_t *data, size_t len)
{
    if (accum->capacity - accum->used < len) {
        return false;
    }

    size_t first = accum->capacity - accum->write_pos;
    if (first > len) {
        first = len;
    }
    memcpy(accum->data + accum->write_pos, data, first);

    size_t remaining = len - first;
    if (remaining > 0) {
        memcpy(accum->data, data + first, remaining);
    }

    accum->write_pos = (accum->write_pos + len) % accum->capacity;
    accum->used += len;
    return true;
}

static bool accum_read(pcm_accum_t *accum, uint8_t *data, size_t len)
{
    if (accum->used < len) {
        return false;
    }

    size_t first = accum->capacity - accum->read_pos;
    if (first > len) {
        first = len;
    }
    memcpy(data, accum->data + accum->read_pos, first);

    size_t remaining = len - first;
    if (remaining > 0) {
        memcpy(data + first, accum->data, remaining);
    }

    accum->read_pos = (accum->read_pos + len) % accum->capacity;
    accum->used -= len;
    return true;
}

static esp_err_t open_audio_path(int output_volume)
{
    ESP_RETURN_ON_ERROR(bsp_audio_open(), TAG, "open audio codec failed");
    ESP_RETURN_ON_ERROR(bsp_audio_set_volume(output_volume), TAG, "set stream volume failed");
    ESP_RETURN_ON_FALSE(BSP_AUDIO_SAMPLE_RATE == AUDIO_OPUS_SAMPLE_RATE, ESP_ERR_NOT_SUPPORTED, TAG, "Opus stream expects 16 kHz audio");
    ESP_RETURN_ON_FALSE(BSP_AUDIO_BITS_PER_SAMPLE == AUDIO_OPUS_BITS_PER_SAMPLE && BSP_AUDIO_CHANNELS == AUDIO_OPUS_CHANNELS,
                        ESP_ERR_NOT_SUPPORTED,
                        TAG,
                        "Opus stream expects 16-bit mono PCM");
    int mute_ret = esp_codec_dev_set_out_mute(bsp_audio_get_codec(), false);
    if (mute_ret != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "unmute stream output failed: %d", mute_ret);
    }
    return ESP_OK;
}

esp_err_t audio_opus_stream_start(const audio_opus_stream_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL && config->send_cb != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid stream config");
    if (s_stream.running) {
        s_stream.send_cb = config->send_cb;
        s_stream.user_ctx = config->user_ctx;
        return ESP_OK;
    }

    memset(&s_stream, 0, sizeof(s_stream));
    s_stream.send_cb = config->send_cb;
    s_stream.user_ctx = config->user_ctx;
    s_stream.log_window_frames = (CONFIG_XIAOZHI_AUDIO_OPUS_STREAM_LOG_WINDOW_MS + AUDIO_OPUS_FRAME_DURATION_MS - 1) / AUDIO_OPUS_FRAME_DURATION_MS;
    if (s_stream.log_window_frames == 0) {
        s_stream.log_window_frames = 1;
    }

    const int volume = config->output_volume >= 0 ? config->output_volume : CONFIG_XIAOZHI_AUDIO_OPUS_STREAM_VOLUME;
    ESP_RETURN_ON_ERROR(open_audio_path(volume), TAG, "prepare stream audio path failed");

    esp_err_t err = audio_opus_codec_open(&s_stream.codec);
    if (err != ESP_OK) {
        return err;
    }

    s_stream.pcm_rb = create_stream_ringbuffer(CONFIG_XIAOZHI_AUDIO_OPUS_STREAM_PCM_RING_BYTES);
    s_stream.downlink_rb = create_stream_ringbuffer(CONFIG_XIAOZHI_AUDIO_OPUS_STREAM_OPUS_RING_BYTES);
    if (s_stream.pcm_rb == NULL || s_stream.downlink_rb == NULL) {
        audio_opus_stream_stop();
        return ESP_ERR_NO_MEM;
    }

    const size_t pcm_max_item = xRingbufferGetMaxItemSize(s_stream.pcm_rb);
    const size_t opus_max_item = xRingbufferGetMaxItemSize(s_stream.downlink_rb);
    if (AUDIO_OPUS_PCM_FRAME_BYTES > pcm_max_item || s_stream.codec.opus_frame_bytes > opus_max_item) {
        ESP_LOGE(TAG,
                 "stream ringbuffer too small pcm_frame=%u pcm_max_item=%u opus_frame=%u opus_max_item=%u",
                 (unsigned int)AUDIO_OPUS_PCM_FRAME_BYTES,
                 (unsigned int)pcm_max_item,
                 (unsigned int)s_stream.codec.opus_frame_bytes,
                 (unsigned int)opus_max_item);
        audio_opus_stream_stop();
        return ESP_ERR_INVALID_SIZE;
    }

    s_stream.running = true;
    err = create_stream_task(decoder_task,
                             "opus_downlink",
                             CONFIG_XIAOZHI_AUDIO_OPUS_STREAM_DECODER_TASK_STACK_SIZE,
                             AUDIO_OPUS_STREAM_DECODER_TASK_PRIORITY,
                             &s_stream.decoder_task);
    if (err == ESP_OK) {
        err = create_stream_task(encoder_task,
                                 "opus_uplink",
                                 CONFIG_XIAOZHI_AUDIO_OPUS_STREAM_ENCODER_TASK_STACK_SIZE,
                                 AUDIO_OPUS_STREAM_ENCODER_TASK_PRIORITY,
                                 &s_stream.encoder_task);
    }
    if (err != ESP_OK) {
        audio_opus_stream_stop();
        return err;
    }

    ESP_LOGI(TAG,
             "opus stream started pcm_frame_bytes=%u opus_capacity=%u pcm_ring=%u opus_ring=%u free heap=%u minimum free heap=%u",
             (unsigned int)s_stream.codec.pcm_frame_bytes,
             (unsigned int)s_stream.codec.opus_frame_bytes,
             (unsigned int)CONFIG_XIAOZHI_AUDIO_OPUS_STREAM_PCM_RING_BYTES,
             (unsigned int)CONFIG_XIAOZHI_AUDIO_OPUS_STREAM_OPUS_RING_BYTES,
             (unsigned int)esp_get_free_heap_size(),
             (unsigned int)esp_get_minimum_free_heap_size());
    return ESP_OK;
}

esp_err_t audio_opus_stream_stop(void)
{
    s_stream.running = false;
    s_stream.uplink_enabled = false;
    drain_ringbuffer(s_stream.pcm_rb);
    drain_ringbuffer(s_stream.downlink_rb);

    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(AUDIO_OPUS_STREAM_SHUTDOWN_WAIT_MS);
    while ((s_stream.encoder_task != NULL || s_stream.decoder_task != NULL) && xTaskGetTickCount() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    if (s_stream.encoder_task != NULL) {
        vTaskDelete(s_stream.encoder_task);
        s_stream.encoder_task = NULL;
    }
    if (s_stream.decoder_task != NULL) {
        vTaskDelete(s_stream.decoder_task);
        s_stream.decoder_task = NULL;
    }

    if (s_stream.pcm_rb != NULL) {
        vRingbufferDelete(s_stream.pcm_rb);
        s_stream.pcm_rb = NULL;
    }
    if (s_stream.downlink_rb != NULL) {
        vRingbufferDelete(s_stream.downlink_rb);
        s_stream.downlink_rb = NULL;
    }

    audio_opus_codec_close(&s_stream.codec);
    s_stream.send_cb = NULL;
    s_stream.user_ctx = NULL;

    ESP_LOGI(TAG,
             "opus stream stopped tx_frames=%u tx_bytes=%u rx_frames=%u decoded_frames=%u playback_failures=%u uplink drop count=%u downlink drop count=%u free heap=%u minimum free heap=%u",
             (unsigned int)s_stream.tx_frames,
             (unsigned int)s_stream.tx_bytes,
             (unsigned int)s_stream.rx_frames,
             (unsigned int)s_stream.decoded_frames,
             (unsigned int)s_stream.playback_failures,
             (unsigned int)s_stream.uplink_drop_count,
             (unsigned int)s_stream.downlink_drop_count,
             (unsigned int)esp_get_free_heap_size(),
             (unsigned int)esp_get_minimum_free_heap_size());
    return ESP_OK;
}

esp_err_t audio_opus_stream_set_uplink_enabled(bool enabled)
{
    if (!s_stream.running) {
        return enabled ? ESP_ERR_INVALID_STATE : ESP_OK;
    }
    s_stream.uplink_enabled = enabled;
    if (!enabled) {
        drain_ringbuffer(s_stream.pcm_rb);
    }
    ESP_LOGI(TAG, "opus uplink %s", enabled ? "enabled" : "disabled");
    return ESP_OK;
}

bool audio_opus_stream_is_uplink_enabled(void)
{
    return s_stream.running && s_stream.uplink_enabled;
}

esp_err_t audio_opus_stream_feed_pcm(const uint8_t *pcm, size_t len)
{
    ESP_RETURN_ON_FALSE(pcm != NULL && len > 0, ESP_ERR_INVALID_ARG, TAG, "invalid uplink pcm");
    if (!s_stream.running || !s_stream.uplink_enabled || s_stream.pcm_rb == NULL) {
        return ESP_OK;
    }

    BaseType_t sent = xRingbufferSend(s_stream.pcm_rb, pcm, len, pdMS_TO_TICKS(AUDIO_OPUS_STREAM_SEND_TIMEOUT_MS));
    if (sent != pdTRUE) {
        s_stream.uplink_drop_count++;
        ESP_LOGW(TAG,
                 "uplink pcm drop len=%u uplink drop count=%u free heap=%u minimum free heap=%u",
                 (unsigned int)len,
                 (unsigned int)s_stream.uplink_drop_count,
                 (unsigned int)esp_get_free_heap_size(),
                 (unsigned int)esp_get_minimum_free_heap_size());
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t audio_opus_stream_enqueue_downlink_opus(const uint8_t *opus, size_t len)
{
    ESP_RETURN_ON_FALSE(opus != NULL && len > 0, ESP_ERR_INVALID_ARG, TAG, "invalid downlink opus");
    if (!s_stream.running || s_stream.downlink_rb == NULL) {
        s_stream.downlink_drop_count++;
        return ESP_ERR_INVALID_STATE;
    }

    BaseType_t sent = xRingbufferSend(s_stream.downlink_rb, opus, len, pdMS_TO_TICKS(AUDIO_OPUS_STREAM_SEND_TIMEOUT_MS));
    if (sent != pdTRUE) {
        s_stream.downlink_drop_count++;
        ESP_LOGW(TAG,
                 "downlink opus drop len=%u downlink drop count=%u free heap=%u minimum free heap=%u",
                 (unsigned int)len,
                 (unsigned int)s_stream.downlink_drop_count,
                 (unsigned int)esp_get_free_heap_size(),
                 (unsigned int)esp_get_minimum_free_heap_size());
        return ESP_ERR_TIMEOUT;
    }

    s_stream.rx_frames++;
    if (should_log_frame(s_stream.rx_frames)) {
        ESP_LOGI(TAG,
                 "downlink opus queued frames=%u len=%u free heap=%u minimum free heap=%u",
                 (unsigned int)s_stream.rx_frames,
                 (unsigned int)len,
                 (unsigned int)esp_get_free_heap_size(),
                 (unsigned int)esp_get_minimum_free_heap_size());
    }
    return ESP_OK;
}

void audio_opus_stream_flush(void)
{
    drain_ringbuffer(s_stream.pcm_rb);
    drain_ringbuffer(s_stream.downlink_rb);
}

static void encoder_task(void *arg)
{
    (void)arg;
    uint8_t *accum_storage = (uint8_t *)alloc_stream_buffer(AUDIO_OPUS_STREAM_ACCUM_BYTES);
    uint8_t *pcm_frame = (uint8_t *)alloc_stream_buffer(s_stream.codec.pcm_frame_bytes);
    uint8_t *opus_frame = (uint8_t *)alloc_stream_buffer(s_stream.codec.opus_frame_bytes);
    pcm_accum_t accum = {0};

    if (accum_storage == NULL || pcm_frame == NULL || opus_frame == NULL) {
        ESP_LOGE(TAG, "allocate encoder stream buffers failed");
        s_stream.running = false;
        goto done;
    }
    accum_reset(&accum, accum_storage, AUDIO_OPUS_STREAM_ACCUM_BYTES);

    while (s_stream.running) {
        size_t item_size = 0;
        uint8_t *item = (uint8_t *)xRingbufferReceive(s_stream.pcm_rb, &item_size, pdMS_TO_TICKS(AUDIO_OPUS_STREAM_RECV_TIMEOUT_MS));
        if (item == NULL) {
            continue;
        }

        if (!s_stream.uplink_enabled) {
            accum.used = 0;
            accum.read_pos = 0;
            accum.write_pos = 0;
            vRingbufferReturnItem(s_stream.pcm_rb, item);
            continue;
        }

        if (!accum_write(&accum, item, item_size)) {
            s_stream.uplink_drop_count++;
            ESP_LOGW(TAG,
                     "uplink accumulator overflow len=%u used=%u uplink drop count=%u",
                     (unsigned int)item_size,
                     (unsigned int)accum.used,
                     (unsigned int)s_stream.uplink_drop_count);
            accum.used = 0;
            accum.read_pos = 0;
            accum.write_pos = 0;
        }
        vRingbufferReturnItem(s_stream.pcm_rb, item);

        while (s_stream.running && s_stream.uplink_enabled && accum.used >= s_stream.codec.pcm_frame_bytes) {
            if (!accum_read(&accum, pcm_frame, s_stream.codec.pcm_frame_bytes)) {
                break;
            }

            size_t opus_len = 0;
            esp_err_t err = audio_opus_codec_encode(&s_stream.codec,
                                                    pcm_frame,
                                                    s_stream.codec.pcm_frame_bytes,
                                                    opus_frame,
                                                    s_stream.codec.opus_frame_bytes,
                                                    &opus_len);
            if (err != ESP_OK) {
                s_stream.uplink_drop_count++;
                ESP_LOGW(TAG, "opus uplink encode failed: %s", esp_err_to_name(err));
                continue;
            }

            err = s_stream.send_cb != NULL ? s_stream.send_cb(opus_frame, opus_len, s_stream.user_ctx) : ESP_ERR_INVALID_STATE;
            if (err != ESP_OK) {
                s_stream.uplink_drop_count++;
                ESP_LOGW(TAG,
                         "opus frame send failed len=%u err=%s uplink drop count=%u",
                         (unsigned int)opus_len,
                         esp_err_to_name(err),
                         (unsigned int)s_stream.uplink_drop_count);
                continue;
            }

            s_stream.tx_frames++;
            s_stream.tx_bytes += opus_len;
            if (should_log_frame(s_stream.tx_frames)) {
                ESP_LOGI(TAG,
                         "opus frame sent len=%u tx_frames=%u encoded bytes=%u uplink drop count=%u free heap=%u minimum free heap=%u",
                         (unsigned int)opus_len,
                         (unsigned int)s_stream.tx_frames,
                         (unsigned int)s_stream.tx_bytes,
                         (unsigned int)s_stream.uplink_drop_count,
                         (unsigned int)esp_get_free_heap_size(),
                         (unsigned int)esp_get_minimum_free_heap_size());
            }
        }
    }

done:
    heap_caps_free(opus_frame);
    heap_caps_free(pcm_frame);
    heap_caps_free(accum_storage);
    s_stream.encoder_task = NULL;
    delete_current_stream_task();
}

static void decoder_task(void *arg)
{
    (void)arg;
    uint8_t *decoded_frame = (uint8_t *)alloc_stream_buffer(s_stream.codec.decoded_frame_bytes);
    if (decoded_frame == NULL) {
        ESP_LOGE(TAG, "allocate decoder stream buffer failed");
        s_stream.running = false;
        goto done;
    }

    while (s_stream.running) {
        size_t item_size = 0;
        uint8_t *item = (uint8_t *)xRingbufferReceive(s_stream.downlink_rb, &item_size, pdMS_TO_TICKS(AUDIO_OPUS_STREAM_RECV_TIMEOUT_MS));
        if (item == NULL) {
            continue;
        }

        size_t decoded_len = 0;
        esp_err_t err = audio_opus_codec_decode(&s_stream.codec,
                                                item,
                                                item_size,
                                                decoded_frame,
                                                s_stream.codec.decoded_frame_bytes,
                                                &decoded_len);
        vRingbufferReturnItem(s_stream.downlink_rb, item);
        if (err != ESP_OK) {
            s_stream.downlink_drop_count++;
            ESP_LOGW(TAG, "downlink opus decode failed: %s", esp_err_to_name(err));
            continue;
        }

        int write_ret = esp_codec_dev_write(bsp_audio_get_codec(), decoded_frame, (int)decoded_len);
        if (write_ret != ESP_CODEC_DEV_OK) {
            s_stream.playback_failures++;
            ESP_LOGW(TAG, "speaker playback failed: %d", write_ret);
            continue;
        }

        s_stream.decoded_frames++;
        if (should_log_frame(s_stream.decoded_frames)) {
            ESP_LOGI(TAG,
                     "decoded pcm bytes=%u decoded_frames=%u playback_failures=%u downlink drop count=%u free heap=%u minimum free heap=%u",
                     (unsigned int)decoded_len,
                     (unsigned int)s_stream.decoded_frames,
                     (unsigned int)s_stream.playback_failures,
                     (unsigned int)s_stream.downlink_drop_count,
                     (unsigned int)esp_get_free_heap_size(),
                     (unsigned int)esp_get_minimum_free_heap_size());
        }
    }

    if (decoded_frame != NULL) {
        memset(decoded_frame, 0, s_stream.codec.decoded_frame_bytes);
        (void)esp_codec_dev_write(bsp_audio_get_codec(), decoded_frame, (int)s_stream.codec.decoded_frame_bytes);
    }

done:
    heap_caps_free(decoded_frame);
    s_stream.decoder_task = NULL;
    delete_current_stream_task();
}
