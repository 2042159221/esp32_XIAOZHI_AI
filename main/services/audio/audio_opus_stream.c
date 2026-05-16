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
#include "freertos/semphr.h"
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

#ifndef CONFIG_XIAOZHI_AUDIO_OPUS_STREAM_CAPTURE_TASK_STACK_SIZE
#define CONFIG_XIAOZHI_AUDIO_OPUS_STREAM_CAPTURE_TASK_STACK_SIZE 8192
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
#define AUDIO_OPUS_STREAM_CAPTURE_IDLE_MS 20
#define AUDIO_OPUS_STREAM_ENCODER_TASK_PRIORITY 6
#define AUDIO_OPUS_STREAM_DECODER_TASK_PRIORITY 5
#define AUDIO_OPUS_STREAM_CAPTURE_TASK_PRIORITY 5

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
    SemaphoreHandle_t codec_lock;
    TaskHandle_t capture_task;
    TaskHandle_t encoder_task;
    TaskHandle_t decoder_task;
    audio_opus_encoder_t encoder;
    audio_opus_decoder_t decoder;
    audio_opus_pcm_source_t pcm_source;
    audio_opus_stream_send_cb_t send_cb;
    void *user_ctx;
    size_t pcm_frame_bytes;
    size_t opus_frame_bytes;
    size_t decoded_frame_bytes;
    int decoder_output_sample_rate;
    volatile bool running;
    volatile bool uplink_enabled;
    uint32_t tx_frames;
    uint32_t tx_bytes;
    uint32_t rx_frames;
    uint32_t decoded_frames;
    volatile uint32_t downlink_pending_frames;
    uint32_t playback_failures;
    uint32_t uplink_drop_count;
    uint32_t downlink_drop_count;
    uint32_t capture_failures;
    uint32_t log_window_frames;
} audio_opus_stream_ctx_t;

static audio_opus_stream_ctx_t s_stream;

static void direct_capture_task(void *arg);
static void encoder_task(void *arg);
static void decoder_task(void *arg);

static size_t pcm_frame_bytes_for_sample_rate(int sample_rate)
{
    return (size_t)((sample_rate * AUDIO_OPUS_CHANNELS * (AUDIO_OPUS_BITS_PER_SAMPLE / 8) * AUDIO_OPUS_FRAME_DURATION_MS) / 1000);
}

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

static void delete_stream_task(TaskHandle_t task)
{
#if CONFIG_SPIRAM
    vTaskDeleteWithCaps(task);
#else
    vTaskDelete(task);
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

static bool lock_codec(TickType_t timeout)
{
    return s_stream.codec_lock != NULL && xSemaphoreTake(s_stream.codec_lock, timeout) == pdTRUE;
}

static void unlock_codec(void)
{
    if (s_stream.codec_lock != NULL) {
        xSemaphoreGive(s_stream.codec_lock);
    }
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

static esp_err_t open_audio_path(int output_volume, int playback_sample_rate)
{
    ESP_RETURN_ON_ERROR(bsp_audio_open_with_sample_rate(playback_sample_rate), TAG, "open audio codec failed");
    ESP_RETURN_ON_ERROR(bsp_audio_set_volume(output_volume), TAG, "set stream volume failed");
    ESP_RETURN_ON_FALSE(BSP_AUDIO_BITS_PER_SAMPLE == AUDIO_OPUS_BITS_PER_SAMPLE && BSP_AUDIO_CHANNELS == AUDIO_OPUS_CHANNELS,
                        ESP_ERR_NOT_SUPPORTED,
                        TAG,
                        "Opus stream expects 16-bit mono PCM");
    int mute_ret = esp_codec_dev_set_out_mute(bsp_audio_get_codec(), false);
    if (mute_ret != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "unmute stream output failed: %d", mute_ret);
    }
    ESP_LOGI(TAG, "audio path ready playback_sample_rate=%d current_codec_sample_rate=%d",
             playback_sample_rate,
             bsp_audio_get_current_sample_rate());
    return ESP_OK;
}

static esp_err_t ensure_encoder_locked(void)
{
    if (s_stream.encoder.encoder != NULL) {
        return ESP_OK;
    }

    audio_opus_decoder_close(&s_stream.decoder);
    esp_err_t err = audio_opus_encoder_open(&s_stream.encoder);
    if (err != ESP_OK) {
        return err;
    }

    s_stream.pcm_frame_bytes = s_stream.encoder.pcm_frame_bytes;
    s_stream.opus_frame_bytes = s_stream.encoder.opus_frame_bytes;
    return ESP_OK;
}

static esp_err_t ensure_decoder_locked(void)
{
    if (s_stream.decoder.decoder != NULL) {
        return ESP_OK;
    }

    audio_opus_encoder_close(&s_stream.encoder);
    esp_err_t err = audio_opus_decoder_open(&s_stream.decoder, s_stream.decoder_output_sample_rate);
    if (err != ESP_OK) {
        return err;
    }

    s_stream.decoded_frame_bytes = s_stream.decoder.decoded_frame_bytes;
    ESP_LOGI(TAG,
             "decoder output sample_rate=%d decoded_frame_bytes=%u",
             s_stream.decoder_output_sample_rate,
             (unsigned int)s_stream.decoded_frame_bytes);
    return ESP_OK;
}

static void close_encoder_locked(void)
{
    audio_opus_encoder_close(&s_stream.encoder);
}

static void close_decoder_locked(void)
{
    audio_opus_decoder_close(&s_stream.decoder);
}

esp_err_t audio_opus_stream_start(const audio_opus_stream_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL && config->send_cb != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid stream config");
    if (s_stream.running) {
        int requested_rate = config->decoder_output_sample_rate > 0 ? config->decoder_output_sample_rate : AUDIO_OPUS_SAMPLE_RATE;
        ESP_RETURN_ON_FALSE(requested_rate == s_stream.decoder_output_sample_rate,
                            ESP_ERR_INVALID_STATE,
                            TAG,
                            "cannot change running stream sample_rate old=%d new=%d",
                            s_stream.decoder_output_sample_rate,
                            requested_rate);
        s_stream.send_cb = config->send_cb;
        s_stream.user_ctx = config->user_ctx;
        return ESP_OK;
    }

    memset(&s_stream, 0, sizeof(s_stream));
    s_stream.send_cb = config->send_cb;
    s_stream.user_ctx = config->user_ctx;
    s_stream.pcm_source = config->pcm_source;
    s_stream.decoder_output_sample_rate = config->decoder_output_sample_rate > 0 ? config->decoder_output_sample_rate : AUDIO_OPUS_SAMPLE_RATE;
    s_stream.pcm_frame_bytes = AUDIO_OPUS_PCM_FRAME_BYTES;
    s_stream.opus_frame_bytes = AUDIO_OPUS_ENCODED_FRAME_CAPACITY_BYTES;
    s_stream.decoded_frame_bytes = pcm_frame_bytes_for_sample_rate(s_stream.decoder_output_sample_rate);
    s_stream.log_window_frames = (CONFIG_XIAOZHI_AUDIO_OPUS_STREAM_LOG_WINDOW_MS + AUDIO_OPUS_FRAME_DURATION_MS - 1) / AUDIO_OPUS_FRAME_DURATION_MS;
    if (s_stream.log_window_frames == 0) {
        s_stream.log_window_frames = 1;
    }

    s_stream.codec_lock = xSemaphoreCreateMutex();
    if (s_stream.codec_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const int volume = config->output_volume >= 0 ? config->output_volume : CONFIG_XIAOZHI_AUDIO_OPUS_STREAM_VOLUME;
    esp_err_t err = open_audio_path(volume, s_stream.decoder_output_sample_rate);
    if (err != ESP_OK) {
        audio_opus_stream_stop();
        return err;
    }

    if (!lock_codec(pdMS_TO_TICKS(1000))) {
        audio_opus_stream_stop();
        return ESP_ERR_TIMEOUT;
    }
    err = ensure_encoder_locked();
    unlock_codec();
    if (err != ESP_OK) {
        audio_opus_stream_stop();
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
    if (s_stream.pcm_frame_bytes > pcm_max_item || s_stream.opus_frame_bytes > opus_max_item) {
        ESP_LOGE(TAG,
                 "stream ringbuffer too small pcm_frame=%u pcm_max_item=%u opus_frame=%u opus_max_item=%u",
                 (unsigned int)s_stream.pcm_frame_bytes,
                 (unsigned int)pcm_max_item,
                 (unsigned int)s_stream.opus_frame_bytes,
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
    if (err == ESP_OK && s_stream.pcm_source == AUDIO_OPUS_PCM_SOURCE_DIRECT_CODEC) {
        err = create_stream_task(direct_capture_task,
                                 "opus_capture",
                                 CONFIG_XIAOZHI_AUDIO_OPUS_STREAM_CAPTURE_TASK_STACK_SIZE,
                                 AUDIO_OPUS_STREAM_CAPTURE_TASK_PRIORITY,
                                 &s_stream.capture_task);
    }
    if (err != ESP_OK) {
        audio_opus_stream_stop();
        return err;
    }

    ESP_LOGI(TAG,
             "opus stream started pcm_source=%d pcm_frame_bytes=%u opus_capacity=%u decoder_output_sample_rate=%d decoded_frame_bytes=%u pcm_ring=%u opus_ring=%u free heap=%u minimum free heap=%u",
             s_stream.pcm_source,
             (unsigned int)s_stream.pcm_frame_bytes,
             (unsigned int)s_stream.opus_frame_bytes,
             s_stream.decoder_output_sample_rate,
             (unsigned int)s_stream.decoded_frame_bytes,
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
    while ((s_stream.capture_task != NULL || s_stream.encoder_task != NULL || s_stream.decoder_task != NULL) &&
           xTaskGetTickCount() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    if (s_stream.capture_task != NULL) {
        delete_stream_task(s_stream.capture_task);
        s_stream.capture_task = NULL;
    }
    if (s_stream.encoder_task != NULL) {
        delete_stream_task(s_stream.encoder_task);
        s_stream.encoder_task = NULL;
    }
    if (s_stream.decoder_task != NULL) {
        delete_stream_task(s_stream.decoder_task);
        s_stream.decoder_task = NULL;
    }

    if (s_stream.codec_lock != NULL && lock_codec(pdMS_TO_TICKS(1000))) {
        close_encoder_locked();
        close_decoder_locked();
        unlock_codec();
    } else {
        audio_opus_encoder_close(&s_stream.encoder);
        audio_opus_decoder_close(&s_stream.decoder);
    }

    if (s_stream.pcm_rb != NULL) {
        vRingbufferDelete(s_stream.pcm_rb);
        s_stream.pcm_rb = NULL;
    }
    if (s_stream.downlink_rb != NULL) {
        vRingbufferDelete(s_stream.downlink_rb);
        s_stream.downlink_rb = NULL;
    }
    if (s_stream.codec_lock != NULL) {
        vSemaphoreDelete(s_stream.codec_lock);
        s_stream.codec_lock = NULL;
    }

    s_stream.send_cb = NULL;
    s_stream.user_ctx = NULL;

    ESP_LOGI(TAG,
             "opus stream stopped tx_frames=%u tx_bytes=%u rx_frames=%u decoded_frames=%u playback_failures=%u capture_failures=%u uplink drop count=%u downlink drop count=%u free heap=%u minimum free heap=%u",
             (unsigned int)s_stream.tx_frames,
             (unsigned int)s_stream.tx_bytes,
             (unsigned int)s_stream.rx_frames,
             (unsigned int)s_stream.decoded_frames,
             (unsigned int)s_stream.playback_failures,
             (unsigned int)s_stream.capture_failures,
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

    esp_err_t err = ESP_OK;
    if (enabled) {
        if (!lock_codec(pdMS_TO_TICKS(1000))) {
            return ESP_ERR_TIMEOUT;
        }
        close_decoder_locked();
        err = ensure_encoder_locked();
        unlock_codec();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "open encoder for uplink failed: %s", esp_err_to_name(err));
            return err;
        }
        s_stream.uplink_enabled = true;
    } else {
        s_stream.uplink_enabled = false;
        drain_ringbuffer(s_stream.pcm_rb);
        if (lock_codec(pdMS_TO_TICKS(1000))) {
            close_encoder_locked();
            unlock_codec();
        } else {
            err = ESP_ERR_TIMEOUT;
        }
    }

    ESP_LOGI(TAG, "opus uplink %s", enabled ? "enabled" : "disabled");
    return err;
}

bool audio_opus_stream_is_uplink_enabled(void)
{
    return s_stream.running && s_stream.uplink_enabled;
}

esp_err_t audio_opus_stream_close_decoder(void)
{
    if (!s_stream.running) {
        return ESP_OK;
    }
    if (!lock_codec(pdMS_TO_TICKS(1000))) {
        return ESP_ERR_TIMEOUT;
    }
    close_decoder_locked();
    unlock_codec();
    ESP_LOGI(TAG, "opus decoder closed");
    return ESP_OK;
}

esp_err_t audio_opus_stream_wait_downlink_idle(uint32_t timeout_ms)
{
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    while (s_stream.running && s_stream.downlink_pending_frames > 0) {
        if (xTaskGetTickCount() >= deadline) {
            ESP_LOGW(TAG, "downlink drain timeout pending_frames=%u", (unsigned int)s_stream.downlink_pending_frames);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(AUDIO_OPUS_STREAM_RECV_TIMEOUT_MS));
    }

    return ESP_OK;
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
                 "uplink pcm backlog drop old frames len=%u uplink drop count=%u free heap=%u minimum free heap=%u",
                 (unsigned int)len,
                 (unsigned int)s_stream.uplink_drop_count,
                 (unsigned int)esp_get_free_heap_size(),
                 (unsigned int)esp_get_minimum_free_heap_size());
        drain_ringbuffer(s_stream.pcm_rb);
        sent = xRingbufferSend(s_stream.pcm_rb, pcm, len, 0);
        return sent == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
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
    s_stream.downlink_pending_frames++;
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

static void direct_capture_task(void *arg)
{
    (void)arg;
    uint8_t *pcm_frame = (uint8_t *)alloc_stream_buffer(s_stream.pcm_frame_bytes);
    if (pcm_frame == NULL) {
        ESP_LOGE(TAG, "allocate direct capture buffer failed");
        s_stream.running = false;
        goto done;
    }

    ESP_LOGI(TAG, "direct codec capture task started pcm_frame_bytes=%u", (unsigned int)s_stream.pcm_frame_bytes);
    while (s_stream.running) {
        if (s_stream.pcm_source != AUDIO_OPUS_PCM_SOURCE_DIRECT_CODEC || !s_stream.uplink_enabled) {
            vTaskDelay(pdMS_TO_TICKS(AUDIO_OPUS_STREAM_CAPTURE_IDLE_MS));
            continue;
        }

        int read_ret = esp_codec_dev_read(bsp_audio_get_codec(), pcm_frame, (int)s_stream.pcm_frame_bytes);
        if (read_ret != ESP_CODEC_DEV_OK) {
            s_stream.capture_failures++;
            ESP_LOGW(TAG, "direct codec capture failed: %d capture_failures=%u", read_ret, (unsigned int)s_stream.capture_failures);
            vTaskDelay(pdMS_TO_TICKS(AUDIO_OPUS_STREAM_CAPTURE_IDLE_MS));
            continue;
        }

        esp_err_t err = audio_opus_stream_feed_pcm(pcm_frame, s_stream.pcm_frame_bytes);
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "direct capture feed pcm failed: %s", esp_err_to_name(err));
        }
    }

done:
    heap_caps_free(pcm_frame);
    s_stream.capture_task = NULL;
    delete_stream_task(NULL);
}

static void encoder_task(void *arg)
{
    (void)arg;
    uint8_t *accum_storage = (uint8_t *)alloc_stream_buffer(AUDIO_OPUS_STREAM_ACCUM_BYTES);
    uint8_t *pcm_frame = (uint8_t *)alloc_stream_buffer(s_stream.pcm_frame_bytes);
    uint8_t *opus_frame = (uint8_t *)alloc_stream_buffer(s_stream.opus_frame_bytes);
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

        while (s_stream.running && s_stream.uplink_enabled && accum.used >= s_stream.pcm_frame_bytes) {
            if (!accum_read(&accum, pcm_frame, s_stream.pcm_frame_bytes)) {
                break;
            }

            if (!lock_codec(pdMS_TO_TICKS(1000))) {
                s_stream.uplink_drop_count++;
                ESP_LOGW(TAG, "opus uplink encode skipped because codec lock timed out");
                continue;
            }

            esp_err_t err = ensure_encoder_locked();
            size_t opus_len = 0;
            if (err == ESP_OK) {
                err = audio_opus_encoder_encode(&s_stream.encoder,
                                                pcm_frame,
                                                s_stream.pcm_frame_bytes,
                                                opus_frame,
                                                s_stream.opus_frame_bytes,
                                                &opus_len);
            }
            unlock_codec();

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
    delete_stream_task(NULL);
}

static void decoder_task(void *arg)
{
    (void)arg;
    uint8_t *decoded_frame = (uint8_t *)alloc_stream_buffer(s_stream.decoded_frame_bytes);
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

        s_stream.uplink_enabled = false;
        drain_ringbuffer(s_stream.pcm_rb);

        if (!lock_codec(pdMS_TO_TICKS(1000))) {
            vRingbufferReturnItem(s_stream.downlink_rb, item);
            s_stream.downlink_drop_count++;
            ESP_LOGW(TAG, "downlink opus decode skipped because codec lock timed out");
            continue;
        }

        esp_err_t err = ensure_decoder_locked();
        size_t decoded_len = 0;
        if (err == ESP_OK) {
            err = audio_opus_decoder_decode(&s_stream.decoder,
                                            item,
                                            item_size,
                                            decoded_frame,
                                            s_stream.decoded_frame_bytes,
                                            &decoded_len);
        }
        unlock_codec();
        vRingbufferReturnItem(s_stream.downlink_rb, item);

        if (err != ESP_OK) {
            s_stream.downlink_drop_count++;
            ESP_LOGW(TAG, "downlink opus decode failed: %s", esp_err_to_name(err));
            continue;
        }

        int write_ret = esp_codec_dev_write(bsp_audio_get_codec(), decoded_frame, (int)decoded_len);
        if (s_stream.downlink_pending_frames > 0) {
            s_stream.downlink_pending_frames--;
        }
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
            ESP_LOGI(TAG, "speaker playback OK");
        }
    }

    if (decoded_frame != NULL) {
        memset(decoded_frame, 0, s_stream.decoded_frame_bytes);
        (void)esp_codec_dev_write(bsp_audio_get_codec(), decoded_frame, (int)s_stream.decoded_frame_bytes);
    }

done:
    heap_caps_free(decoded_frame);
    s_stream.decoder_task = NULL;
    delete_stream_task(NULL);
}
