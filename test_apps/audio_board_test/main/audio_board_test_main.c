#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "audio_opus_diag.h"
#include "audio_pcm_diag.h"
#include "audio_pcm_service.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_platform_init.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "wifi_sta_service.h"

static const char *TAG = "audio_board_test";

#ifndef CONFIG_AUDIO_BOARD_TEST_RUN_I2C_SCAN
#define CONFIG_AUDIO_BOARD_TEST_RUN_I2C_SCAN 0
#endif

#ifndef CONFIG_AUDIO_BOARD_TEST_RUN_1KHZ_TONE
#define CONFIG_AUDIO_BOARD_TEST_RUN_1KHZ_TONE 0
#endif

#ifndef CONFIG_AUDIO_BOARD_TEST_RUN_MIC_RMS
#define CONFIG_AUDIO_BOARD_TEST_RUN_MIC_RMS 0
#endif

#ifndef CONFIG_AUDIO_BOARD_TEST_RUN_LOCAL_LOOPBACK
#define CONFIG_AUDIO_BOARD_TEST_RUN_LOCAL_LOOPBACK 0
#endif

#ifndef CONFIG_AUDIO_BOARD_TEST_RUN_OPUS_1KHZ_LOOPBACK
#define CONFIG_AUDIO_BOARD_TEST_RUN_OPUS_1KHZ_LOOPBACK 0
#endif

#ifndef CONFIG_AUDIO_BOARD_TEST_RUN_OPUS_MIC_LOOPBACK
#define CONFIG_AUDIO_BOARD_TEST_RUN_OPUS_MIC_LOOPBACK 0
#endif

#ifndef CONFIG_AUDIO_BOARD_TEST_RUN_WS_ECHO
#define CONFIG_AUDIO_BOARD_TEST_RUN_WS_ECHO 0
#endif

#ifndef CONFIG_AUDIO_BOARD_TEST_TASK_STACK_SIZE
#define CONFIG_AUDIO_BOARD_TEST_TASK_STACK_SIZE 12288
#endif

#ifndef CONFIG_AUDIO_BOARD_TEST_WS_URI
#define CONFIG_AUDIO_BOARD_TEST_WS_URI ""
#endif

#ifndef CONFIG_AUDIO_BOARD_TEST_WS_DURATION_MS
#define CONFIG_AUDIO_BOARD_TEST_WS_DURATION_MS 10000
#endif

#ifndef CONFIG_AUDIO_BOARD_TEST_WS_TOKEN
#define CONFIG_AUDIO_BOARD_TEST_WS_TOKEN ""
#endif

enum {
    WS_CONNECTED_BIT = BIT0,
    WS_STOPPED_BIT = BIT1,
    WS_ERROR_BIT = BIT2,
};

#define AUDIO_BOARD_TEST_TASK_PRIORITY 5

#if CONFIG_AUDIO_BOARD_TEST_RUN_WS_ECHO
typedef struct {
    EventGroupHandle_t events;
    esp_websocket_client_handle_t client;
    size_t tx_frames;
    size_t tx_bytes;
    size_t rx_frames;
    size_t rx_bytes;
    size_t dropped_rx_frames;
} audio_board_ws_ctx_t;

static audio_board_ws_ctx_t s_ws_ctx;

static void audio_board_ws_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);

static void audio_board_ws_cleanup(audio_board_ws_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }

    audio_pcm_service_stop_stream();

    if (ctx->client != NULL) {
        esp_websocket_client_close(ctx->client, pdMS_TO_TICKS(1000));
        esp_websocket_client_stop(ctx->client);
        esp_websocket_unregister_events(ctx->client, WEBSOCKET_EVENT_ANY, audio_board_ws_event_handler);
        esp_websocket_client_destroy(ctx->client);
        ctx->client = NULL;
    }

    if (ctx->events != NULL) {
        vEventGroupDelete(ctx->events);
        ctx->events = NULL;
    }
}

static esp_err_t audio_board_ws_send_pcm(const uint8_t *data, size_t len, void *user_ctx)
{
    audio_board_ws_ctx_t *ctx = (audio_board_ws_ctx_t *)user_ctx;
    if (ctx == NULL || ctx->client == NULL || !esp_websocket_client_is_connected(ctx->client)) {
        return ESP_ERR_INVALID_STATE;
    }

    int sent = esp_websocket_client_send_bin(ctx->client, (const char *)data, (int)len, pdMS_TO_TICKS(200));
    if (sent != (int)len) {
        return ESP_FAIL;
    }

    ctx->tx_frames++;
    ctx->tx_bytes += len;
    return ESP_OK;
}

static void audio_board_ws_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)base;

    audio_board_ws_ctx_t *ctx = (audio_board_ws_ctx_t *)handler_args;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    if (ctx == NULL) {
        return;
    }

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket connected");
        if (audio_pcm_service_start_stream(audio_board_ws_send_pcm, ctx) != ESP_OK) {
            ESP_LOGE(TAG, "failed to start PCM capture/playback stream");
            xEventGroupSetBits(ctx->events, WS_ERROR_BIT);
            break;
        }
        xEventGroupSetBits(ctx->events, WS_CONNECTED_BIT);
        break;
    case WEBSOCKET_EVENT_DATA:
        if (data->op_code == 0x2 && data->data_len > 0) {
            ctx->rx_frames++;
            ctx->rx_bytes += (size_t)data->data_len;
            esp_err_t err = audio_pcm_service_enqueue_playback((const uint8_t *)data->data_ptr, (size_t)data->data_len);
            if (err != ESP_OK) {
                ctx->dropped_rx_frames++;
                ESP_LOGW(TAG, "drop echoed PCM frame len=%d err=%s", data->data_len, esp_err_to_name(err));
            }

            if (ctx->rx_frames <= 3 || (ctx->rx_frames % 50) == 0) {
                ESP_LOGI(TAG, "echo frame rx count=%u len=%d total_rx_bytes=%u dropped=%u",
                         (unsigned int)ctx->rx_frames,
                         data->data_len,
                         (unsigned int)ctx->rx_bytes,
                         (unsigned int)ctx->dropped_rx_frames);
            }
        } else {
            ESP_LOGI(TAG, "non-binary websocket frame opcode=%d len=%d", data->op_code, data->data_len);
        }
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WebSocket disconnected, http_status=%d", data->error_handle.esp_ws_handshake_status_code);
        audio_pcm_service_stop_stream();
        xEventGroupSetBits(ctx->events, WS_STOPPED_BIT);
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket error, http_status=%d tls=0x%x sock_errno=0x%x",
                 data->error_handle.esp_ws_handshake_status_code,
                 data->error_handle.esp_tls_last_esp_err,
                 data->error_handle.esp_transport_sock_errno);
        audio_pcm_service_stop_stream();
        xEventGroupSetBits(ctx->events, WS_ERROR_BIT);
        break;
    case WEBSOCKET_EVENT_CLOSED:
        ESP_LOGI(TAG, "WebSocket closed");
        audio_pcm_service_stop_stream();
        xEventGroupSetBits(ctx->events, WS_STOPPED_BIT);
        break;
    default:
        break;
    }
}

static esp_err_t run_ws_echo_test(void)
{
    ESP_RETURN_ON_FALSE(CONFIG_AUDIO_BOARD_TEST_WS_URI[0] != '\0', ESP_ERR_INVALID_ARG, TAG, "WebSocket URI is empty");
    ESP_RETURN_ON_ERROR(wifi_sta_service_connect_default(), TAG, "connect Wi-Fi for WS echo failed");

    memset(&s_ws_ctx, 0, sizeof(s_ws_ctx));
    s_ws_ctx.events = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_ws_ctx.events != NULL, ESP_ERR_NO_MEM, TAG, "create WebSocket event group failed");

    const esp_websocket_client_config_t websocket_cfg = {
        .uri = CONFIG_AUDIO_BOARD_TEST_WS_URI,
        .network_timeout_ms = 10000,
        .reconnect_timeout_ms = 5000,
        .buffer_size = 2048,
        .task_stack = 6144,
        .task_prio = 6,
    };

    s_ws_ctx.client = esp_websocket_client_init(&websocket_cfg);
    if (s_ws_ctx.client == NULL) {
        audio_board_ws_cleanup(&s_ws_ctx);
        return ESP_ERR_NO_MEM;
    }

    if (CONFIG_AUDIO_BOARD_TEST_WS_TOKEN[0] != '\0') {
        esp_err_t err = esp_websocket_client_append_header(s_ws_ctx.client, "Authorization", CONFIG_AUDIO_BOARD_TEST_WS_TOKEN);
        if (err != ESP_OK) {
            audio_board_ws_cleanup(&s_ws_ctx);
            return err;
        }
    }

    esp_err_t err = esp_websocket_register_events(s_ws_ctx.client, WEBSOCKET_EVENT_ANY, audio_board_ws_event_handler, &s_ws_ctx);
    if (err != ESP_OK) {
        audio_board_ws_cleanup(&s_ws_ctx);
        return err;
    }

    err = esp_websocket_client_start(s_ws_ctx.client);
    if (err != ESP_OK) {
        audio_board_ws_cleanup(&s_ws_ctx);
        return err;
    }

    EventBits_t bits = xEventGroupWaitBits(s_ws_ctx.events, WS_CONNECTED_BIT | WS_ERROR_BIT | WS_STOPPED_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));
    if ((bits & WS_CONNECTED_BIT) == 0) {
        audio_board_ws_cleanup(&s_ws_ctx);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "start PCM echo test uri=%s duration_ms=%u", CONFIG_AUDIO_BOARD_TEST_WS_URI, (unsigned int)CONFIG_AUDIO_BOARD_TEST_WS_DURATION_MS);
    uint32_t elapsed_ms = 0;
    while (elapsed_ms < CONFIG_AUDIO_BOARD_TEST_WS_DURATION_MS) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        elapsed_ms += 1000;

        bits = xEventGroupGetBits(s_ws_ctx.events);
        if ((bits & (WS_ERROR_BIT | WS_STOPPED_BIT)) != 0) {
            ESP_LOGE(TAG, "WS echo test aborted after %u ms", (unsigned int)elapsed_ms);
            audio_board_ws_cleanup(&s_ws_ctx);
            return ESP_FAIL;
        }

        ESP_LOGI(TAG,
                 "WS echo stats elapsed_ms=%u tx_frames=%u tx_bytes=%u rx_frames=%u rx_bytes=%u dropped_rx=%u",
                 (unsigned int)elapsed_ms,
                 (unsigned int)s_ws_ctx.tx_frames,
                 (unsigned int)s_ws_ctx.tx_bytes,
                 (unsigned int)s_ws_ctx.rx_frames,
                 (unsigned int)s_ws_ctx.rx_bytes,
                 (unsigned int)s_ws_ctx.dropped_rx_frames);
    }

    ESP_LOGI(TAG,
             "WS echo test finished tx_frames=%u tx_bytes=%u rx_frames=%u rx_bytes=%u dropped_rx=%u",
             (unsigned int)s_ws_ctx.tx_frames,
             (unsigned int)s_ws_ctx.tx_bytes,
             (unsigned int)s_ws_ctx.rx_frames,
             (unsigned int)s_ws_ctx.rx_bytes,
             (unsigned int)s_ws_ctx.dropped_rx_frames);

    audio_board_ws_cleanup(&s_ws_ctx);
    return ESP_OK;
}
#endif

static esp_err_t run_audio_board_test(void)
{
#if CONFIG_AUDIO_BOARD_TEST_RUN_I2C_SCAN
    ESP_RETURN_ON_ERROR(audio_diag_i2c_scan(), TAG, "audio I2C scan failed");
#endif

#if CONFIG_AUDIO_BOARD_TEST_RUN_1KHZ_TONE
    ESP_RETURN_ON_ERROR(audio_diag_play_1khz_tone(), TAG, "1 kHz tone test failed");
#endif

#if CONFIG_AUDIO_BOARD_TEST_RUN_MIC_RMS
    ESP_RETURN_ON_ERROR(audio_diag_print_mic_rms(), TAG, "mic RMS test failed");
#endif

#if CONFIG_AUDIO_BOARD_TEST_RUN_LOCAL_LOOPBACK
    ESP_RETURN_ON_ERROR(audio_diag_loopback(), TAG, "local loopback test failed");
#endif

#if CONFIG_AUDIO_BOARD_TEST_RUN_OPUS_1KHZ_LOOPBACK
    ESP_RETURN_ON_ERROR(audio_opus_diag_play_1khz_loopback(), TAG, "Opus 1 kHz loopback test failed");
#endif

#if CONFIG_AUDIO_BOARD_TEST_RUN_OPUS_MIC_LOOPBACK
    ESP_RETURN_ON_ERROR(audio_opus_diag_mic_loopback(), TAG, "Opus mic loopback test failed");
#endif

#if CONFIG_AUDIO_BOARD_TEST_RUN_WS_ECHO
    ESP_RETURN_ON_ERROR(run_ws_echo_test(), TAG, "WS echo test failed");
#endif

    return ESP_OK;
}

static void audio_board_test_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "audio board test task start stack_size=%d", CONFIG_AUDIO_BOARD_TEST_TASK_STACK_SIZE);
    esp_err_t err = run_audio_board_test();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio board test failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "audio board test completed successfully");
    }
    ESP_LOGI(TAG, "audio board test task stack high watermark=%u", (unsigned int)uxTaskGetStackHighWaterMark(NULL));
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "audio board test app boot");
    ESP_LOGI(TAG,
             "config: i2c_scan=%d tone=%d mic_rms=%d local_loopback=%d opus_1khz=%d opus_mic=%d ws_echo=%d",
             CONFIG_AUDIO_BOARD_TEST_RUN_I2C_SCAN,
             CONFIG_AUDIO_BOARD_TEST_RUN_1KHZ_TONE,
             CONFIG_AUDIO_BOARD_TEST_RUN_MIC_RMS,
             CONFIG_AUDIO_BOARD_TEST_RUN_LOCAL_LOOPBACK,
             CONFIG_AUDIO_BOARD_TEST_RUN_OPUS_1KHZ_LOOPBACK,
             CONFIG_AUDIO_BOARD_TEST_RUN_OPUS_MIC_LOOPBACK,
             CONFIG_AUDIO_BOARD_TEST_RUN_WS_ECHO);

    esp_err_t err = esp_platform_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "platform init failed: %s", esp_err_to_name(err));
        return;
    }

    BaseType_t created = xTaskCreate(audio_board_test_task,
                                     "audio_board_test",
                                     CONFIG_AUDIO_BOARD_TEST_TASK_STACK_SIZE,
                                     NULL,
                                     AUDIO_BOARD_TEST_TASK_PRIORITY,
                                     NULL);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "create audio board test task failed");
        return;
    }
}
