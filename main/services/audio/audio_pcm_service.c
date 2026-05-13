#include "audio_pcm_service.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "bsp_audio.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"

static const char *TAG = "audio_pcm";

#define AUDIO_PCM_FRAME_MS 20
#define AUDIO_PCM_FRAME_BYTES ((BSP_AUDIO_SAMPLE_RATE * BSP_AUDIO_BITS_PER_SAMPLE / 8 * BSP_AUDIO_CHANNELS * AUDIO_PCM_FRAME_MS) / 1000)
#define AUDIO_PCM_RING_BYTES (AUDIO_PCM_FRAME_BYTES * 24)
#define AUDIO_PCM_TASK_STACK 4096
#define AUDIO_PCM_CAPTURE_TASK_PRIORITY 6
#define AUDIO_PCM_PLAYBACK_TASK_PRIORITY 5
#define AUDIO_PCM_DEFAULT_VOLUME 65

static RingbufHandle_t s_playback_rb;
static TaskHandle_t s_capture_task;
static TaskHandle_t s_playback_task;
static uint8_t *s_capture_frame;
static audio_pcm_tx_cb_t s_tx_cb;
static void *s_tx_user_ctx;
static volatile bool s_stream_running;

static RingbufHandle_t create_playback_ringbuffer(void)
{
#if CONFIG_SPIRAM
    RingbufHandle_t ringbuffer = xRingbufferCreateWithCaps(AUDIO_PCM_RING_BYTES, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ringbuffer != NULL) {
        return ringbuffer;
    }
#endif

    return xRingbufferCreateWithCaps(AUDIO_PCM_RING_BYTES, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_8BIT);
}

static void drain_playback_ringbuffer(void)
{
    if (s_playback_rb == NULL) {
        return;
    }

    while (true) {
        size_t item_size = 0;
        void *item = xRingbufferReceive(s_playback_rb, &item_size, 0);
        if (item == NULL) {
            break;
        }
        vRingbufferReturnItem(s_playback_rb, item);
    }
}

static void playback_task(void *arg)
{
    (void)arg;

    while (s_stream_running) {
        size_t item_size = 0;
        uint8_t *item = (uint8_t *)xRingbufferReceive(s_playback_rb, &item_size, pdMS_TO_TICKS(100));
        if (item == NULL) {
            continue;
        }

        int ret = esp_codec_dev_write(bsp_audio_get_codec(), item, (int)item_size);
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "playback write failed: %d", ret);
        }
        vRingbufferReturnItem(s_playback_rb, item);
    }

    s_playback_task = NULL;
    vTaskDelete(NULL);
}

static void capture_task(void *arg)
{
    (void)arg;
    if (s_capture_frame == NULL) {
        ESP_LOGE(TAG, "capture frame is not ready");
        s_stream_running = false;
        s_capture_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    while (s_stream_running) {
        int ret = esp_codec_dev_read(bsp_audio_get_codec(), s_capture_frame, AUDIO_PCM_FRAME_BYTES);
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "capture read failed: %d", ret);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (s_tx_cb != NULL) {
            esp_err_t err = s_tx_cb(s_capture_frame, AUDIO_PCM_FRAME_BYTES, s_tx_user_ctx);
            if (err != ESP_OK) {
                ESP_LOGD(TAG, "PCM frame send skipped: %s", esp_err_to_name(err));
            }
        }
    }

    s_capture_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t audio_pcm_service_init(void)
{
    ESP_RETURN_ON_ERROR(bsp_audio_open(), TAG, "open audio codec failed");
    ESP_RETURN_ON_ERROR(bsp_audio_set_volume(AUDIO_PCM_DEFAULT_VOLUME), TAG, "set default volume failed");

    if (s_playback_rb == NULL) {
        s_playback_rb = create_playback_ringbuffer();
        ESP_RETURN_ON_FALSE(s_playback_rb != NULL, ESP_ERR_NO_MEM, TAG, "create playback ringbuffer failed");
    }

    if (s_capture_frame == NULL) {
        s_capture_frame = (uint8_t *)heap_caps_malloc(AUDIO_PCM_FRAME_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        ESP_RETURN_ON_FALSE(s_capture_frame != NULL, ESP_ERR_NO_MEM, TAG, "alloc capture frame failed");
    }

    return ESP_OK;
}

esp_err_t audio_pcm_service_start_stream(audio_pcm_tx_cb_t tx_cb, void *user_ctx)
{
    ESP_RETURN_ON_ERROR(audio_pcm_service_init(), TAG, "init before stream failed");
    if (s_stream_running) {
        return ESP_OK;
    }

    s_tx_cb = tx_cb;
    s_tx_user_ctx = user_ctx;
    s_stream_running = true;
    drain_playback_ringbuffer();

    BaseType_t created = xTaskCreate(capture_task, "pcm_capture", AUDIO_PCM_TASK_STACK, NULL, AUDIO_PCM_CAPTURE_TASK_PRIORITY, &s_capture_task);
    ESP_RETURN_ON_FALSE(created == pdPASS, ESP_ERR_NO_MEM, TAG, "create capture task failed");

    created = xTaskCreate(playback_task, "pcm_playback", AUDIO_PCM_TASK_STACK, NULL, AUDIO_PCM_PLAYBACK_TASK_PRIORITY, &s_playback_task);
    if (created != pdPASS) {
        s_stream_running = false;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "PCM stream started: %d Hz, %d bit, mono", BSP_AUDIO_SAMPLE_RATE, BSP_AUDIO_BITS_PER_SAMPLE);
    return ESP_OK;
}

esp_err_t audio_pcm_service_stop_stream(void)
{
    s_stream_running = false;
    s_tx_cb = NULL;
    s_tx_user_ctx = NULL;
    if (s_playback_rb != NULL) {
        drain_playback_ringbuffer();
    }
    return ESP_OK;
}

esp_err_t audio_pcm_service_enqueue_playback(const uint8_t *data, size_t len)
{
    ESP_RETURN_ON_FALSE(data != NULL && len > 0, ESP_ERR_INVALID_ARG, TAG, "invalid playback data");
    ESP_RETURN_ON_FALSE(s_playback_rb != NULL, ESP_ERR_INVALID_STATE, TAG, "playback ringbuffer is not ready");
    BaseType_t sent = xRingbufferSend(s_playback_rb, data, len, 0);
    return sent == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}
