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
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "freertos/ringbuf.h"
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

#ifndef CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_SOURCE_TASK_STACK_SIZE
#define CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_SOURCE_TASK_STACK_SIZE 6144
#endif

#ifndef CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_ENCODER_TASK_STACK_SIZE
#define CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_ENCODER_TASK_STACK_SIZE 49152
#endif

#ifndef CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_BRIDGE_TASK_STACK_SIZE
#define CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_BRIDGE_TASK_STACK_SIZE 4096
#endif

#ifndef CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_DECODER_TASK_STACK_SIZE
#define CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_DECODER_TASK_STACK_SIZE 24576
#endif

#ifndef CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_PCM_RING_BYTES
#define CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_PCM_RING_BYTES 32768
#endif

#ifndef CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_OPUS_RING_BYTES
#define CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_OPUS_RING_BYTES 32768
#endif

#define AUDIO_OPUS_1KHZ_PERIOD_SAMPLES 16
#define AUDIO_OPUS_LOCAL_ACCUM_RING_BYTES (AUDIO_OPUS_PCM_FRAME_BYTES * 4)
#define AUDIO_OPUS_PACKET_HEADER_BYTES 4
#define AUDIO_OPUS_STACK_WARN_BYTES 4096
#define AUDIO_OPUS_RING_SEND_TIMEOUT_MS 1000
#define AUDIO_OPUS_RING_RECV_TIMEOUT_MS 1000
#define AUDIO_OPUS_PIPELINE_SHUTDOWN_MS 3000
#define AUDIO_OPUS_SOURCE_TASK_PRIORITY 5
#define AUDIO_OPUS_ENCODER_TASK_PRIORITY 6
#define AUDIO_OPUS_BRIDGE_TASK_PRIORITY 5
#define AUDIO_OPUS_DECODER_TASK_PRIORITY 5

enum {
    AUDIO_OPUS_EVENT_SOURCE_DONE = BIT0,
    AUDIO_OPUS_EVENT_ENCODER_DONE = BIT1,
    AUDIO_OPUS_EVENT_BRIDGE_DONE = BIT2,
    AUDIO_OPUS_EVENT_DECODER_DONE = BIT3,
    AUDIO_OPUS_EVENT_ERROR = BIT4,
};

#define AUDIO_OPUS_EVENT_ALL_DONE \
    (AUDIO_OPUS_EVENT_SOURCE_DONE | AUDIO_OPUS_EVENT_ENCODER_DONE | AUDIO_OPUS_EVENT_BRIDGE_DONE | AUDIO_OPUS_EVENT_DECODER_DONE)

typedef enum {
    AUDIO_OPUS_DIAG_SOURCE_TONE = 0,
    AUDIO_OPUS_DIAG_SOURCE_MIC,
} audio_opus_diag_source_t;

typedef struct __attribute__((packed)) {
    uint16_t len;
    uint16_t seq;
} audio_opus_packet_header_t;

typedef struct {
    uint8_t *data;
    size_t capacity;
    size_t read_pos;
    size_t write_pos;
    size_t used;
} audio_opus_pcm_accum_t;

typedef struct {
    RingbufHandle_t pcm_rb;
    RingbufHandle_t opus_tx_rb;
    RingbufHandle_t opus_rx_rb;
    EventGroupHandle_t events;
    audio_opus_codec_t codec;
    audio_opus_diag_source_t source;
    volatile bool running;
    esp_err_t first_error;
    uint32_t duration_ms;
    uint32_t total_frames;
    uint32_t log_window_frames;
} audio_opus_pipeline_t;

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

static RingbufHandle_t create_diag_ringbuffer(size_t size)
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

static void delete_current_diag_task(void)
{
#if CONFIG_SPIRAM
    vTaskDeleteWithCaps(NULL);
#else
    vTaskDelete(NULL);
#endif
}

static esp_err_t create_diag_task(TaskFunction_t task_fn, const char *name, uint32_t stack_size, UBaseType_t priority, void *arg)
{
#if CONFIG_SPIRAM
    BaseType_t created = xTaskCreatePinnedToCoreWithCaps(task_fn,
                                                         name,
                                                         stack_size,
                                                         arg,
                                                         priority,
                                                         NULL,
                                                         tskNO_AFFINITY,
                                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    BaseType_t created = xTaskCreate(task_fn, name, stack_size, arg, priority, NULL);
#endif
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

static const char *source_name(audio_opus_diag_source_t source)
{
    return source == AUDIO_OPUS_DIAG_SOURCE_MIC ? "mic" : "tone";
}

static bool should_log_frame(uint32_t frame_index, uint32_t log_window_frames)
{
    return frame_index < 3 || ((frame_index + 1) % log_window_frames) == 0;
}

static void log_stack_warning(const char *task_name, UBaseType_t watermark)
{
    if (watermark < AUDIO_OPUS_STACK_WARN_BYTES) {
        ESP_LOGW(TAG, "%s stack watermark low=%u bytes", task_name, (unsigned int)watermark);
    }
}

static void log_pipeline_stats(const char *task_name,
                               const char *source,
                               uint32_t frame_index,
                               size_t pcm_bytes,
                               size_t opus_bytes,
                               size_t decoded_bytes)
{
    UBaseType_t watermark = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(TAG,
             "%s source=%s frame=%u frame_samples=%u pcm input bytes=%u opus encoded bytes=%u decoded pcm bytes=%u free heap=%u internal heap=%u minimum free heap=%u task stack watermark=%u",
             task_name,
             source,
             (unsigned int)(frame_index + 1),
             (unsigned int)(AUDIO_OPUS_PCM_FRAME_BYTES / sizeof(int16_t)),
             (unsigned int)pcm_bytes,
             (unsigned int)opus_bytes,
             (unsigned int)decoded_bytes,
             (unsigned int)esp_get_free_heap_size(),
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned int)esp_get_minimum_free_heap_size(),
             (unsigned int)watermark);
    log_stack_warning(task_name, watermark);
}

static void set_pipeline_error(audio_opus_pipeline_t *ctx, esp_err_t err, const char *message)
{
    if (ctx->first_error == ESP_OK) {
        ctx->first_error = err;
    }
    ctx->running = false;
    ESP_LOGE(TAG, "%s: %s", message, esp_err_to_name(err));
    xEventGroupSetBits(ctx->events, AUDIO_OPUS_EVENT_ERROR);
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

static void accum_reset(audio_opus_pcm_accum_t *accum, uint8_t *storage, size_t capacity)
{
    accum->data = storage;
    accum->capacity = capacity;
    accum->read_pos = 0;
    accum->write_pos = 0;
    accum->used = 0;
}

static bool accum_write(audio_opus_pcm_accum_t *accum, const uint8_t *data, size_t len)
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

static bool accum_read(audio_opus_pcm_accum_t *accum, uint8_t *data, size_t len)
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

static bool send_pcm_frame(audio_opus_pipeline_t *ctx, const uint8_t *pcm_frame, uint32_t frame_index)
{
    BaseType_t sent = xRingbufferSend(ctx->pcm_rb, pcm_frame, ctx->codec.pcm_frame_bytes, pdMS_TO_TICKS(AUDIO_OPUS_RING_SEND_TIMEOUT_MS));
    if (sent != pdTRUE) {
        set_pipeline_error(ctx, ESP_ERR_TIMEOUT, "send PCM frame to encoder timed out");
        return false;
    }

    if (should_log_frame(frame_index, ctx->log_window_frames)) {
        log_pipeline_stats("pcm_source_task", source_name(ctx->source), frame_index, ctx->codec.pcm_frame_bytes, 0, 0);
    }
    return true;
}

static void audio_pcm_source_task(void *arg)
{
    audio_opus_pipeline_t *ctx = (audio_opus_pipeline_t *)arg;
    uint8_t *pcm_frame = (uint8_t *)alloc_diag_buffer(ctx->codec.pcm_frame_bytes);
    uint8_t *mic_chunk = NULL;
    uint8_t *accum_storage = NULL;
    audio_opus_pcm_accum_t accum = {0};

    if (pcm_frame == NULL) {
        set_pipeline_error(ctx, ESP_ERR_NO_MEM, "allocate source PCM frame failed");
        goto done;
    }

    ESP_LOGI(TAG,
             "audio_pcm_source_task start source=%s total_frames=%u pcm_frame_bytes=%u",
             source_name(ctx->source),
             (unsigned int)ctx->total_frames,
             (unsigned int)ctx->codec.pcm_frame_bytes);

    if (ctx->source == AUDIO_OPUS_DIAG_SOURCE_TONE) {
        for (uint32_t frame_index = 0; ctx->running && frame_index < ctx->total_frames; ++frame_index) {
            fill_1khz_opus_frame(pcm_frame, frame_index);
            if (!send_pcm_frame(ctx, pcm_frame, frame_index)) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(AUDIO_OPUS_FRAME_DURATION_MS));
        }
    } else {
        mic_chunk = (uint8_t *)alloc_diag_buffer(CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_MIC_INPUT_BYTES);
        accum_storage = (uint8_t *)alloc_diag_buffer(AUDIO_OPUS_LOCAL_ACCUM_RING_BYTES);
        if (mic_chunk == NULL || accum_storage == NULL) {
            set_pipeline_error(ctx, ESP_ERR_NO_MEM, "allocate mic source buffers failed");
            goto done;
        }
        accum_reset(&accum, accum_storage, AUDIO_OPUS_LOCAL_ACCUM_RING_BYTES);

        uint32_t frame_index = 0;
        while (ctx->running && frame_index < ctx->total_frames) {
            int read_ret = esp_codec_dev_read(bsp_audio_get_codec(), mic_chunk, CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_MIC_INPUT_BYTES);
            if (read_ret != ESP_CODEC_DEV_OK) {
                set_pipeline_error(ctx, ESP_FAIL, "mic opus source capture failed");
                break;
            }

            if (should_log_frame(frame_index, ctx->log_window_frames)) {
                ESP_LOGI(TAG,
                         "mic read bytes=%u local accum used=%u free heap=%u internal heap=%u",
                         (unsigned int)CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_MIC_INPUT_BYTES,
                         (unsigned int)accum.used,
                         (unsigned int)esp_get_free_heap_size(),
                         (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
            }

            if (!accum_write(&accum, mic_chunk, CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_MIC_INPUT_BYTES)) {
                set_pipeline_error(ctx, ESP_ERR_NO_MEM, "mic opus local accumulator overflow");
                break;
            }

            while (ctx->running && accum.used >= ctx->codec.pcm_frame_bytes && frame_index < ctx->total_frames) {
                if (!accum_read(&accum, pcm_frame, ctx->codec.pcm_frame_bytes)) {
                    set_pipeline_error(ctx, ESP_FAIL, "mic opus local accumulator read failed");
                    break;
                }
                ESP_LOGI(TAG, "ring buffer accumulated bytes=%u", (unsigned int)ctx->codec.pcm_frame_bytes);
                if (!send_pcm_frame(ctx, pcm_frame, frame_index)) {
                    break;
                }
                frame_index++;
            }
        }
    }

done:
    free_diag_buffer(accum_storage);
    free_diag_buffer(mic_chunk);
    free_diag_buffer(pcm_frame);
    ESP_LOGI(TAG, "audio_pcm_source_task done source=%s", source_name(ctx->source));
    xEventGroupSetBits(ctx->events, AUDIO_OPUS_EVENT_SOURCE_DONE);
    delete_current_diag_task();
}

static void audio_encoder_task(void *arg)
{
    audio_opus_pipeline_t *ctx = (audio_opus_pipeline_t *)arg;
    uint8_t *pcm_frame = (uint8_t *)alloc_diag_buffer(ctx->codec.pcm_frame_bytes);
    uint8_t *packet = (uint8_t *)alloc_diag_buffer(AUDIO_OPUS_PACKET_HEADER_BYTES + ctx->codec.opus_frame_bytes);

    if (pcm_frame == NULL || packet == NULL) {
        set_pipeline_error(ctx, ESP_ERR_NO_MEM, "allocate encoder buffers failed");
        goto done;
    }

    ESP_LOGI(TAG,
             "audio_encoder_task start total_frames=%u pcm_ring=%u opus_tx_ring=%u stack=%u",
             (unsigned int)ctx->total_frames,
             (unsigned int)CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_PCM_RING_BYTES,
             (unsigned int)CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_OPUS_RING_BYTES,
             (unsigned int)CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_ENCODER_TASK_STACK_SIZE);

    for (uint32_t frame_index = 0; ctx->running && frame_index < ctx->total_frames; ++frame_index) {
        size_t item_size = 0;
        uint8_t *item = (uint8_t *)xRingbufferReceive(ctx->pcm_rb, &item_size, pdMS_TO_TICKS(AUDIO_OPUS_RING_RECV_TIMEOUT_MS));
        if (item == NULL) {
            if (ctx->running) {
                set_pipeline_error(ctx, ESP_ERR_TIMEOUT, "encoder timed out waiting for PCM frame");
            }
            break;
        }

        if (item_size != ctx->codec.pcm_frame_bytes) {
            vRingbufferReturnItem(ctx->pcm_rb, item);
            set_pipeline_error(ctx, ESP_ERR_INVALID_SIZE, "encoder received invalid PCM frame size");
            break;
        }

        memcpy(pcm_frame, item, item_size);
        vRingbufferReturnItem(ctx->pcm_rb, item);

        audio_opus_packet_header_t *header = (audio_opus_packet_header_t *)packet;
        size_t opus_len = 0;
        esp_err_t ret = audio_opus_codec_encode(&ctx->codec,
                                                pcm_frame,
                                                ctx->codec.pcm_frame_bytes,
                                                packet + AUDIO_OPUS_PACKET_HEADER_BYTES,
                                                ctx->codec.opus_frame_bytes,
                                                &opus_len);
        if (ret != ESP_OK) {
            set_pipeline_error(ctx, ret, "encoder failed to encode Opus frame");
            break;
        }

        if (opus_len > UINT16_MAX) {
            set_pipeline_error(ctx, ESP_ERR_INVALID_SIZE, "encoder Opus packet too large");
            break;
        }

        header->len = (uint16_t)opus_len;
        header->seq = (uint16_t)frame_index;
        BaseType_t sent = xRingbufferSend(ctx->opus_tx_rb,
                                          packet,
                                          AUDIO_OPUS_PACKET_HEADER_BYTES + opus_len,
                                          pdMS_TO_TICKS(AUDIO_OPUS_RING_SEND_TIMEOUT_MS));
        if (sent != pdTRUE) {
            set_pipeline_error(ctx, ESP_ERR_TIMEOUT, "send Opus packet to bridge timed out");
            break;
        }

        if (should_log_frame(frame_index, ctx->log_window_frames)) {
            log_pipeline_stats("audio_encoder_task", source_name(ctx->source), frame_index, ctx->codec.pcm_frame_bytes, opus_len, 0);
        }
    }

done:
    free_diag_buffer(packet);
    free_diag_buffer(pcm_frame);
    ESP_LOGI(TAG, "audio_encoder_task done source=%s", source_name(ctx->source));
    xEventGroupSetBits(ctx->events, AUDIO_OPUS_EVENT_ENCODER_DONE);
    delete_current_diag_task();
}

static void audio_loopback_bridge_task(void *arg)
{
    audio_opus_pipeline_t *ctx = (audio_opus_pipeline_t *)arg;

    ESP_LOGI(TAG, "audio_loopback_bridge_task start total_frames=%u", (unsigned int)ctx->total_frames);

    for (uint32_t frame_index = 0; ctx->running && frame_index < ctx->total_frames; ++frame_index) {
        size_t item_size = 0;
        uint8_t *item = (uint8_t *)xRingbufferReceive(ctx->opus_tx_rb, &item_size, pdMS_TO_TICKS(AUDIO_OPUS_RING_RECV_TIMEOUT_MS));
        if (item == NULL) {
            if (ctx->running) {
                set_pipeline_error(ctx, ESP_ERR_TIMEOUT, "bridge timed out waiting for Opus packet");
            }
            break;
        }

        if (item_size < AUDIO_OPUS_PACKET_HEADER_BYTES) {
            vRingbufferReturnItem(ctx->opus_tx_rb, item);
            set_pipeline_error(ctx, ESP_ERR_INVALID_SIZE, "bridge received short Opus packet");
            break;
        }

        audio_opus_packet_header_t *header = (audio_opus_packet_header_t *)item;
        const uint16_t packet_len = header->len;
        const uint16_t packet_seq = header->seq;
        if ((size_t)packet_len + AUDIO_OPUS_PACKET_HEADER_BYTES != item_size) {
            vRingbufferReturnItem(ctx->opus_tx_rb, item);
            set_pipeline_error(ctx, ESP_ERR_INVALID_SIZE, "bridge received mismatched Opus packet length");
            break;
        }

        BaseType_t sent = xRingbufferSend(ctx->opus_rx_rb, item, item_size, pdMS_TO_TICKS(AUDIO_OPUS_RING_SEND_TIMEOUT_MS));
        vRingbufferReturnItem(ctx->opus_tx_rb, item);
        if (sent != pdTRUE) {
            set_pipeline_error(ctx, ESP_ERR_TIMEOUT, "bridge send to decoder timed out");
            break;
        }

        if (should_log_frame(frame_index, ctx->log_window_frames)) {
            ESP_LOGI(TAG,
                     "loopback bridge seq=%u opus packet bytes=%u free heap=%u internal heap=%u task stack watermark=%u",
                     (unsigned int)packet_seq,
                     (unsigned int)item_size,
                     (unsigned int)esp_get_free_heap_size(),
                     (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                     (unsigned int)uxTaskGetStackHighWaterMark(NULL));
        }
    }

    ESP_LOGI(TAG, "audio_loopback_bridge_task done source=%s", source_name(ctx->source));
    xEventGroupSetBits(ctx->events, AUDIO_OPUS_EVENT_BRIDGE_DONE);
    delete_current_diag_task();
}

static void audio_decoder_task(void *arg)
{
    audio_opus_pipeline_t *ctx = (audio_opus_pipeline_t *)arg;
    uint8_t *decoded_frame = (uint8_t *)alloc_diag_buffer(ctx->codec.decoded_frame_bytes);

    if (decoded_frame == NULL) {
        set_pipeline_error(ctx, ESP_ERR_NO_MEM, "allocate decoder buffer failed");
        goto done;
    }

    ESP_LOGI(TAG,
             "audio_decoder_task start total_frames=%u opus_rx_ring=%u stack=%u",
             (unsigned int)ctx->total_frames,
             (unsigned int)CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_OPUS_RING_BYTES,
             (unsigned int)CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_DECODER_TASK_STACK_SIZE);

    for (uint32_t frame_index = 0; ctx->running && frame_index < ctx->total_frames; ++frame_index) {
        size_t item_size = 0;
        uint8_t *item = (uint8_t *)xRingbufferReceive(ctx->opus_rx_rb, &item_size, pdMS_TO_TICKS(AUDIO_OPUS_RING_RECV_TIMEOUT_MS));
        if (item == NULL) {
            if (ctx->running) {
                set_pipeline_error(ctx, ESP_ERR_TIMEOUT, "decoder timed out waiting for Opus packet");
            }
            break;
        }

        if (item_size < AUDIO_OPUS_PACKET_HEADER_BYTES) {
            vRingbufferReturnItem(ctx->opus_rx_rb, item);
            set_pipeline_error(ctx, ESP_ERR_INVALID_SIZE, "decoder received short Opus packet");
            break;
        }

        audio_opus_packet_header_t *header = (audio_opus_packet_header_t *)item;
        size_t opus_len = header->len;
        if (opus_len == 0 || opus_len + AUDIO_OPUS_PACKET_HEADER_BYTES != item_size) {
            vRingbufferReturnItem(ctx->opus_rx_rb, item);
            set_pipeline_error(ctx, ESP_ERR_INVALID_SIZE, "decoder received invalid Opus packet length");
            break;
        }

        size_t decoded_len = 0;
        esp_err_t ret = audio_opus_codec_decode(&ctx->codec,
                                                item + AUDIO_OPUS_PACKET_HEADER_BYTES,
                                                opus_len,
                                                decoded_frame,
                                                ctx->codec.decoded_frame_bytes,
                                                &decoded_len);
        vRingbufferReturnItem(ctx->opus_rx_rb, item);
        if (ret != ESP_OK) {
            set_pipeline_error(ctx, ret, "decoder failed to decode Opus frame");
            break;
        }

        if (should_log_frame(frame_index, ctx->log_window_frames)) {
            log_pipeline_stats("audio_decoder_task", source_name(ctx->source), frame_index, ctx->codec.pcm_frame_bytes, opus_len, decoded_len);
        }

        int write_ret = esp_codec_dev_write(bsp_audio_get_codec(), decoded_frame, (int)decoded_len);
        if (write_ret != ESP_CODEC_DEV_OK) {
            set_pipeline_error(ctx, ESP_FAIL, "decoder playback write failed");
            break;
        }
    }

    memset(decoded_frame, 0, ctx->codec.decoded_frame_bytes);
    (void)esp_codec_dev_write(bsp_audio_get_codec(), decoded_frame, (int)ctx->codec.decoded_frame_bytes);

done:
    free_diag_buffer(decoded_frame);
    ESP_LOGI(TAG, "audio_decoder_task done source=%s", source_name(ctx->source));
    xEventGroupSetBits(ctx->events, AUDIO_OPUS_EVENT_DECODER_DONE);
    delete_current_diag_task();
}

static esp_err_t wait_for_pipeline(audio_opus_pipeline_t *ctx, uint32_t timeout_ms)
{
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);

    while (true) {
        EventBits_t bits = xEventGroupGetBits(ctx->events);
        if ((bits & AUDIO_OPUS_EVENT_ERROR) != 0) {
            return ctx->first_error == ESP_OK ? ESP_FAIL : ctx->first_error;
        }
        if ((bits & AUDIO_OPUS_EVENT_ALL_DONE) == AUDIO_OPUS_EVENT_ALL_DONE) {
            return ESP_OK;
        }
        if ((xTaskGetTickCount() - start) > timeout_ticks) {
            ctx->running = false;
            return ESP_ERR_TIMEOUT;
        }

        (void)xEventGroupWaitBits(ctx->events,
                                  AUDIO_OPUS_EVENT_ALL_DONE | AUDIO_OPUS_EVENT_ERROR,
                                  pdFALSE,
                                  pdFALSE,
                                  pdMS_TO_TICKS(200));
    }
}

static void wait_for_pipeline_shutdown(audio_opus_pipeline_t *ctx)
{
    (void)xEventGroupWaitBits(ctx->events,
                              AUDIO_OPUS_EVENT_ALL_DONE,
                              pdFALSE,
                              pdTRUE,
                              pdMS_TO_TICKS(AUDIO_OPUS_PIPELINE_SHUTDOWN_MS));
}

static esp_err_t start_pipeline_tasks(audio_opus_pipeline_t *ctx)
{
    ESP_RETURN_ON_ERROR(create_diag_task(audio_decoder_task,
                                         "opus_decoder",
                                         CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_DECODER_TASK_STACK_SIZE,
                                         AUDIO_OPUS_DECODER_TASK_PRIORITY,
                                         ctx),
                        TAG,
                        "create decoder task failed");
    ESP_RETURN_ON_ERROR(create_diag_task(audio_loopback_bridge_task,
                                         "opus_bridge",
                                         CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_BRIDGE_TASK_STACK_SIZE,
                                         AUDIO_OPUS_BRIDGE_TASK_PRIORITY,
                                         ctx),
                        TAG,
                        "create bridge task failed");
    ESP_RETURN_ON_ERROR(create_diag_task(audio_encoder_task,
                                         "opus_encoder",
                                         CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_ENCODER_TASK_STACK_SIZE,
                                         AUDIO_OPUS_ENCODER_TASK_PRIORITY,
                                         ctx),
                        TAG,
                        "create encoder task failed");
    ESP_RETURN_ON_ERROR(create_diag_task(audio_pcm_source_task,
                                         "opus_pcm_source",
                                         CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_SOURCE_TASK_STACK_SIZE,
                                         AUDIO_OPUS_SOURCE_TASK_PRIORITY,
                                         ctx),
                        TAG,
                        "create PCM source task failed");
    return ESP_OK;
}

static esp_err_t run_opus_pipeline(audio_opus_diag_source_t source, uint32_t duration_ms)
{
    ESP_RETURN_ON_ERROR(open_audio_for_opus_diag(), TAG, "prepare audio codec failed");

    audio_opus_pipeline_t ctx = {
        .source = source,
        .running = true,
        .first_error = ESP_OK,
        .duration_ms = duration_ms,
    };
    esp_err_t ret = audio_opus_codec_open(&ctx.codec);
    if (ret != ESP_OK) {
        return ret;
    }

    ctx.total_frames = (duration_ms + AUDIO_OPUS_FRAME_DURATION_MS - 1) / AUDIO_OPUS_FRAME_DURATION_MS;
    if (ctx.total_frames == 0) {
        ctx.total_frames = 1;
    }
    ctx.log_window_frames = (CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_LOG_WINDOW_MS + AUDIO_OPUS_FRAME_DURATION_MS - 1) / AUDIO_OPUS_FRAME_DURATION_MS;
    if (ctx.log_window_frames == 0) {
        ctx.log_window_frames = 1;
    }

    ctx.events = xEventGroupCreate();
    ctx.pcm_rb = create_diag_ringbuffer(CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_PCM_RING_BYTES);
    ctx.opus_tx_rb = create_diag_ringbuffer(CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_OPUS_RING_BYTES);
    ctx.opus_rx_rb = create_diag_ringbuffer(CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_OPUS_RING_BYTES);
    if (ctx.events == NULL || ctx.pcm_rb == NULL || ctx.opus_tx_rb == NULL || ctx.opus_rx_rb == NULL) {
        ret = ESP_ERR_NO_MEM;
        ESP_LOGE(TAG, "create Opus pipeline synchronization objects failed");
        goto cleanup;
    }

    const size_t pcm_rb_max_item = xRingbufferGetMaxItemSize(ctx.pcm_rb);
    const size_t opus_tx_rb_max_item = xRingbufferGetMaxItemSize(ctx.opus_tx_rb);
    const size_t opus_rx_rb_max_item = xRingbufferGetMaxItemSize(ctx.opus_rx_rb);

    if (ctx.codec.pcm_frame_bytes > pcm_rb_max_item) {
        ESP_LOGE(TAG,
                 "PCM ringbuffer item too small frame=%u max_item=%u ring_bytes=%u",
                 (unsigned int)ctx.codec.pcm_frame_bytes,
                 (unsigned int)pcm_rb_max_item,
                 (unsigned int)CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_PCM_RING_BYTES);
        ret = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }
    if (ctx.codec.opus_frame_bytes + AUDIO_OPUS_PACKET_HEADER_BYTES > opus_tx_rb_max_item ||
        ctx.codec.opus_frame_bytes + AUDIO_OPUS_PACKET_HEADER_BYTES > opus_rx_rb_max_item) {
        ESP_LOGE(TAG,
                 "Opus ringbuffer item too small packet=%u tx_max_item=%u rx_max_item=%u ring_bytes=%u",
                 (unsigned int)(ctx.codec.opus_frame_bytes + AUDIO_OPUS_PACKET_HEADER_BYTES),
                 (unsigned int)opus_tx_rb_max_item,
                 (unsigned int)opus_rx_rb_max_item,
                 (unsigned int)CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_OPUS_RING_BYTES);
        ret = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    int mute_ret = esp_codec_dev_set_out_mute(bsp_audio_get_codec(), false);
    if (mute_ret != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "unmute output before opus pipeline failed: %d", mute_ret);
    }

    ESP_LOGI(TAG,
             "start %s PCM -> encoder task -> loopback bridge -> decoder task -> speaker duration_ms=%u frame_ms=%u pcm_frame_bytes=%u pcm_ring=%u opus_ring=%u pcm_max_item=%u opus_max_item=%u source_stack=%u encoder_stack=%u bridge_stack=%u decoder_stack=%u",
             source_name(source),
             (unsigned int)duration_ms,
             (unsigned int)AUDIO_OPUS_FRAME_DURATION_MS,
             (unsigned int)ctx.codec.pcm_frame_bytes,
             (unsigned int)CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_PCM_RING_BYTES,
             (unsigned int)CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_OPUS_RING_BYTES,
             (unsigned int)pcm_rb_max_item,
             (unsigned int)opus_tx_rb_max_item,
             (unsigned int)CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_SOURCE_TASK_STACK_SIZE,
             (unsigned int)CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_ENCODER_TASK_STACK_SIZE,
             (unsigned int)CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_BRIDGE_TASK_STACK_SIZE,
             (unsigned int)CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_DECODER_TASK_STACK_SIZE);

    ret = start_pipeline_tasks(&ctx);
    if (ret == ESP_OK) {
        uint32_t timeout_ms = duration_ms + (ctx.total_frames * AUDIO_OPUS_FRAME_DURATION_MS) + AUDIO_OPUS_PIPELINE_SHUTDOWN_MS;
        ret = wait_for_pipeline(&ctx, timeout_ms);
    }

    ctx.running = false;
    wait_for_pipeline_shutdown(&ctx);

cleanup:
    (void)esp_codec_dev_set_out_mute(bsp_audio_get_codec(), true);
    if (ctx.pcm_rb != NULL) {
        vRingbufferDelete(ctx.pcm_rb);
    }
    if (ctx.opus_tx_rb != NULL) {
        vRingbufferDelete(ctx.opus_tx_rb);
    }
    if (ctx.opus_rx_rb != NULL) {
        vRingbufferDelete(ctx.opus_rx_rb);
    }
    if (ctx.events != NULL) {
        vEventGroupDelete(ctx.events);
    }
    audio_opus_codec_close(&ctx.codec);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "%s opus async pipeline finished frames=%u", source_name(source), (unsigned int)ctx.total_frames);
    } else {
        ESP_LOGE(TAG, "%s opus async pipeline failed: %s", source_name(source), esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t audio_opus_diag_play_1khz_loopback(void)
{
    return run_opus_pipeline(AUDIO_OPUS_DIAG_SOURCE_TONE, CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_TONE_LOOPBACK_MS);
}

esp_err_t audio_opus_diag_mic_loopback(void)
{
    ESP_RETURN_ON_FALSE((CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_MIC_INPUT_BYTES % sizeof(int16_t)) == 0,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "mic input bytes must be aligned to 16-bit samples");
    ESP_RETURN_ON_FALSE(CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_MIC_INPUT_BYTES > 0 &&
                            CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_MIC_INPUT_BYTES <= AUDIO_OPUS_LOCAL_ACCUM_RING_BYTES,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "invalid mic input bytes");
    return run_opus_pipeline(AUDIO_OPUS_DIAG_SOURCE_MIC, CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_MIC_LOOPBACK_MS);
}
