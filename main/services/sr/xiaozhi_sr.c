#include "xiaozhi_sr.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "bsp_audio.h"
#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "xiaozhi_sr";

#define XIAOZHI_SR_MODEL_PARTITION "model"
#define XIAOZHI_SR_WAKE_HINT "himiaomiao"
#define XIAOZHI_SR_TASK_STACK_FEED 4096
#define XIAOZHI_SR_TASK_STACK_DETECT 6144
#define XIAOZHI_SR_TASK_PRIORITY_FEED 6
#define XIAOZHI_SR_TASK_PRIORITY_DETECT 5
#define XIAOZHI_SR_FETCH_TIMEOUT_MS 100

static const esp_afe_sr_iface_t *s_afe_handle;
static esp_afe_sr_data_t *s_afe_data;
static afe_config_t *s_afe_config;
static srmodel_list_t *s_models;
static TaskHandle_t s_feed_task;
static TaskHandle_t s_detect_task;
static int16_t *s_feed_buffer;
static size_t s_feed_buffer_bytes;
static int s_feed_chunksize;
static int s_feed_channel_num;
static volatile bool s_running;
static volatile bool s_wake_flag;
static xiaozhi_sr_callbacks_t s_callbacks;
static vad_state_t s_last_vad_state = VAD_SILENCE;
static char *s_wakenet_model_name;
static char *s_vad_model_name;

static void sr_feed_task(void *arg);
static void sr_detect_task(void *arg);

static void reset_state(void)
{
    s_afe_handle = NULL;
    s_afe_data = NULL;
    s_afe_config = NULL;
    s_models = NULL;
    s_feed_task = NULL;
    s_detect_task = NULL;
    s_feed_buffer = NULL;
    s_feed_buffer_bytes = 0;
    s_feed_chunksize = 0;
    s_feed_channel_num = 0;
    s_running = false;
    s_wake_flag = false;
    s_last_vad_state = VAD_SILENCE;
    s_wakenet_model_name = NULL;
    s_vad_model_name = NULL;
    memset(&s_callbacks, 0, sizeof(s_callbacks));
}

static void cleanup_resources(void)
{
    if (s_afe_handle != NULL && s_afe_data != NULL) {
        s_afe_handle->destroy(s_afe_data);
    }
    s_afe_data = NULL;
    s_afe_handle = NULL;

    if (s_afe_config != NULL) {
        afe_config_free(s_afe_config);
    }
    s_afe_config = NULL;

    if (s_models != NULL) {
        esp_srmodel_deinit(s_models);
    }
    s_models = NULL;
    s_wakenet_model_name = NULL;
    s_vad_model_name = NULL;

    if (s_feed_buffer != NULL) {
        heap_caps_free(s_feed_buffer);
    }
    s_feed_buffer = NULL;
    s_feed_buffer_bytes = 0;
    s_feed_chunksize = 0;
    s_feed_channel_num = 0;
}

static void notify_vad_state(vad_state_t state)
{
    if (s_callbacks.vad_state_cb != NULL) {
        s_callbacks.vad_state_cb(state, s_callbacks.user_ctx);
    }
}

static void notify_wake(void)
{
    if (s_callbacks.wake_cb != NULL) {
        s_callbacks.wake_cb(s_callbacks.user_ctx);
    }
}

static esp_err_t select_models(void)
{
    s_models = esp_srmodel_init(XIAOZHI_SR_MODEL_PARTITION);
    ESP_RETURN_ON_FALSE(s_models != NULL, ESP_ERR_NOT_FOUND, TAG, "init sr models from partition failed");

    s_wakenet_model_name = esp_srmodel_filter(s_models, "wn", XIAOZHI_SR_WAKE_HINT);
    if (s_wakenet_model_name == NULL) {
        s_wakenet_model_name = esp_srmodel_filter(s_models, "wn", NULL);
    }
    ESP_RETURN_ON_FALSE(s_wakenet_model_name != NULL, ESP_ERR_NOT_FOUND, TAG, "no WakeNet model found in model partition");

    s_vad_model_name = esp_srmodel_filter(s_models, "vadnet", NULL);
    ESP_RETURN_ON_FALSE(s_vad_model_name != NULL, ESP_ERR_NOT_FOUND, TAG, "no VADNet model found in model partition");

    char *wake_words = esp_srmodel_get_wake_words(s_models, s_wakenet_model_name);
    ESP_LOGI(TAG, "selected WakeNet model: %s", s_wakenet_model_name);
    ESP_LOGI(TAG, "selected wake words: %s", wake_words != NULL ? wake_words : "<unknown>");
    ESP_LOGI(TAG, "selected VAD model: %s", s_vad_model_name);
    return ESP_OK;
}

static esp_err_t init_afe(void)
{
    s_afe_config = afe_config_init("M", s_models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    ESP_RETURN_ON_FALSE(s_afe_config != NULL, ESP_ERR_NO_MEM, TAG, "alloc afe config failed");

    s_afe_config->aec_init = false;
    s_afe_config->se_init = false;
    s_afe_config->ns_init = false;
    s_afe_config->agc_init = false;
    s_afe_config->vad_init = true;
    s_afe_config->wakenet_init = true;
    s_afe_config->wakenet_model_name = s_wakenet_model_name;
    s_afe_config->vad_model_name = s_vad_model_name;
    s_afe_config->wakenet_mode = DET_MODE_90;
    s_afe_config->vad_mode = VAD_MODE_1;
    s_afe_config->vad_min_noise_ms = 1000;
    s_afe_config->vad_min_speech_ms = 128;
    s_afe_config->vad_delay_ms = 128;
    s_afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    s_afe_config->fixed_first_channel = true;
    s_afe_config->fixed_output_channel = true;
    s_afe_config->output_playback_channel = false;

    s_afe_config = afe_config_check(s_afe_config);
    afe_config_print(s_afe_config);

    s_afe_handle = esp_afe_handle_from_config(s_afe_config);
    ESP_RETURN_ON_FALSE(s_afe_handle != NULL, ESP_ERR_INVALID_STATE, TAG, "get afe handle failed");

    s_afe_data = s_afe_handle->create_from_config(s_afe_config);
    ESP_RETURN_ON_FALSE(s_afe_data != NULL, ESP_ERR_NO_MEM, TAG, "create afe instance failed");

    s_afe_handle->print_pipeline(s_afe_data);

    s_feed_chunksize = s_afe_handle->get_feed_chunksize(s_afe_data);
    s_feed_channel_num = s_afe_handle->get_feed_channel_num(s_afe_data);
    int fetch_chunksize = s_afe_handle->get_fetch_chunksize(s_afe_data);
    int sample_rate = s_afe_handle->get_samp_rate(s_afe_data);

    s_feed_buffer_bytes = (size_t)s_feed_chunksize * (size_t)s_feed_channel_num * sizeof(int16_t);
    s_feed_buffer = (int16_t *)heap_caps_malloc(s_feed_buffer_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(s_feed_buffer != NULL, ESP_ERR_NO_MEM, TAG, "alloc feed buffer failed");

    ESP_LOGI(TAG, "SR init OK");
    ESP_LOGI(TAG, "feed_chunksize=%d", s_feed_chunksize);
    ESP_LOGI(TAG, "feed channel num=%d", s_feed_channel_num);
    ESP_LOGI(TAG, "fetch_chunksize=%d", fetch_chunksize);
    ESP_LOGI(TAG, "sample_rate=%d", sample_rate);
    return ESP_OK;
}

static void sr_feed_task(void *arg)
{
    (void)arg;

    while (s_running) {
        int ret = esp_codec_dev_read(bsp_audio_get_codec(), s_feed_buffer, (int)s_feed_buffer_bytes);
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "sr mic read failed: %d", ret);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        int feed_ret = s_afe_handle->feed(s_afe_data, s_feed_buffer);
        if (feed_ret <= 0) {
            ESP_LOGW(TAG, "afe feed failed: %d", feed_ret);
        }
    }

    s_feed_task = NULL;
    vTaskDelete(NULL);
}

static void sr_detect_task(void *arg)
{
    (void)arg;

    while (s_running) {
        afe_fetch_result_t *result = s_afe_handle->fetch_with_delay(s_afe_data, pdMS_TO_TICKS(XIAOZHI_SR_FETCH_TIMEOUT_MS));
        if (result == NULL) {
            continue;
        }

        if (result->wakeup_state == WAKENET_DETECTED) {
            s_wake_flag = true;
            ESP_LOGI(TAG, "检测到唤醒词: model=%s index=%d", s_wakenet_model_name, result->wake_word_index);
            notify_wake();
        }

        if (result->vad_state != s_last_vad_state) {
            s_last_vad_state = result->vad_state;
            if (result->vad_state == VAD_SPEECH) {
                ESP_LOGI(TAG, "检测到人声");
            } else {
                ESP_LOGI(TAG, "检测到静音");
            }
            notify_vad_state(result->vad_state);
        }
    }

    s_detect_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t xiaozhi_sr_init(const xiaozhi_sr_callbacks_t *callbacks)
{
    if (s_running) {
        ESP_LOGW(TAG, "sr service already running");
        return ESP_OK;
    }

    reset_state();
    if (callbacks != NULL) {
        s_callbacks = *callbacks;
    }

    ESP_RETURN_ON_ERROR(bsp_audio_open(), TAG, "open audio codec failed");

    if (!esp_psram_is_initialized()) {
        ESP_RETURN_ON_FALSE(false, ESP_ERR_INVALID_STATE, TAG, "PSRAM is required for current SR configuration");
    }

    ESP_LOGI(TAG, "psram size=%u", (unsigned int)esp_psram_get_size());

    esp_err_t err = select_models();
    if (err != ESP_OK) {
        cleanup_resources();
        return err;
    }

    err = init_afe();
    if (err != ESP_OK) {
        cleanup_resources();
        return err;
    }

    s_running = true;

    BaseType_t created = xTaskCreate(sr_feed_task, "sr_feed", XIAOZHI_SR_TASK_STACK_FEED, NULL, XIAOZHI_SR_TASK_PRIORITY_FEED, &s_feed_task);
    if (created != pdPASS) {
        s_running = false;
        cleanup_resources();
        return ESP_ERR_NO_MEM;
    }

    created = xTaskCreate(sr_detect_task, "sr_detect", XIAOZHI_SR_TASK_STACK_DETECT, NULL, XIAOZHI_SR_TASK_PRIORITY_DETECT, &s_detect_task);
    if (created != pdPASS) {
        s_running = false;
        for (int i = 0; i < 10 && s_feed_task != NULL; ++i) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (s_feed_task != NULL) {
            vTaskDelete(s_feed_task);
            s_feed_task = NULL;
        }
        cleanup_resources();
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t xiaozhi_sr_stop(void)
{
    s_running = false;

    for (int i = 0; i < 20 && (s_feed_task != NULL || s_detect_task != NULL); ++i) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    if (s_feed_task != NULL) {
        vTaskDelete(s_feed_task);
        s_feed_task = NULL;
    }
    if (s_detect_task != NULL) {
        vTaskDelete(s_detect_task);
        s_detect_task = NULL;
    }

    cleanup_resources();
    s_wake_flag = false;
    s_last_vad_state = VAD_SILENCE;
    memset(&s_callbacks, 0, sizeof(s_callbacks));
    return ESP_OK;
}

bool xiaozhi_sr_get_wake_flag(void)
{
    return s_wake_flag;
}

void xiaozhi_sr_clear_wake_flag(void)
{
    s_wake_flag = false;
}
