#include "xiaozhi_ws.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "audio_pcm_service.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "xiaozhi_handle.h"

static const char *TAG = "xiaozhi_ws";

static xiaozhi_ws_state_t s_ws_state = XIAOZHI_WS_STATE_IDLE;
static esp_websocket_client_handle_t s_ws_client;
static bool s_local_audio_checked;

#ifndef CONFIG_XIAOZHI_AUDIO_BOOT_LOOPBACK_TEST
#define CONFIG_XIAOZHI_AUDIO_BOOT_LOOPBACK_TEST 0
#endif

static void log_token_summary(const char *token)
{
    if (token == NULL) {
        ESP_LOGI(TAG, "websocket token: <null>");
        return;
    }

    size_t len = strlen(token);
    ESP_LOGI(TAG, "websocket token present, len=%u", (unsigned int)len);
}

static esp_err_t send_pcm_frame(const uint8_t *data, size_t len, void *user_ctx)
{
    esp_websocket_client_handle_t client = (esp_websocket_client_handle_t)user_ctx;
    if (client == NULL || !esp_websocket_client_is_connected(client)) {
        return ESP_ERR_INVALID_STATE;
    }

    int sent = esp_websocket_client_send_bin(client, (const char *)data, (int)len, pdMS_TO_TICKS(200));
    return sent == (int)len ? ESP_OK : ESP_FAIL;
}

static void log_error_if_nonzero(const char *message, int error_code)
{
    if (error_code != 0) {
        ESP_LOGE(TAG, "%s: 0x%x", message, error_code);
    }
}

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "websocket connected");
        s_ws_state = XIAOZHI_WS_STATE_CONNECTED;
        if (audio_pcm_service_start_stream(send_pcm_frame, s_ws_client) != ESP_OK) {
            ESP_LOGE(TAG, "start PCM websocket stream failed");
            s_ws_state = XIAOZHI_WS_STATE_ERROR;
        }
        break;
    case WEBSOCKET_EVENT_DATA:
        if (data->op_code == 0x2 && data->data_len > 0) {
            esp_err_t err = audio_pcm_service_enqueue_playback((const uint8_t *)data->data_ptr, (size_t)data->data_len);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "drop inbound PCM frame: %s", esp_err_to_name(err));
            }
        } else {
            ESP_LOGI(TAG, "websocket non-audio frame opcode=%d len=%d", data->op_code, data->data_len);
        }
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "websocket disconnected");
        log_error_if_nonzero("HTTP status", data->error_handle.esp_ws_handshake_status_code);
        audio_pcm_service_stop_stream();
        s_ws_state = XIAOZHI_WS_STATE_DISCONNECTED;
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "websocket error");
        log_error_if_nonzero("HTTP status", data->error_handle.esp_ws_handshake_status_code);
        if (data->error_handle.error_type == WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT) {
            log_error_if_nonzero("esp-tls", data->error_handle.esp_tls_last_esp_err);
            log_error_if_nonzero("tls stack", data->error_handle.esp_tls_stack_err);
            log_error_if_nonzero("socket errno", data->error_handle.esp_transport_sock_errno);
        }
        audio_pcm_service_stop_stream();
        s_ws_state = XIAOZHI_WS_STATE_ERROR;
        break;
    case WEBSOCKET_EVENT_CLOSED:
        ESP_LOGI(TAG, "websocket closed");
        audio_pcm_service_stop_stream();
        s_ws_state = XIAOZHI_WS_STATE_DISCONNECTED;
        break;
    default:
        break;
    }
}

esp_err_t xiaozhi_ws_start(void)
{
    if (!xiaozhi_handle_is_activated()) {
        ESP_LOGW(TAG, "skip websocket start because device is not activated");
        s_ws_state = XIAOZHI_WS_STATE_ERROR;
        return ESP_ERR_INVALID_STATE;
    }

    const char *url = xiaozhi_handle_get_websocket_url();
    const char *token = xiaozhi_handle_get_websocket_token();
    if (url == NULL || url[0] == '\0' || token == NULL || token[0] == '\0') {
        ESP_LOGW(TAG, "skip websocket start because url or token is missing");
        s_ws_state = XIAOZHI_WS_STATE_ERROR;
        return ESP_ERR_INVALID_STATE;
    }

    if (s_ws_client != NULL) {
        ESP_LOGW(TAG, "websocket client already started");
        return ESP_OK;
    }

    esp_err_t err = audio_pcm_service_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init PCM audio service failed: %s", esp_err_to_name(err));
        s_ws_state = XIAOZHI_WS_STATE_ERROR;
        return err;
    }

    if (!s_local_audio_checked && CONFIG_XIAOZHI_AUDIO_BOOT_LOOPBACK_TEST) {
        err = audio_pcm_service_local_loopback(1200);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "local PCM loopback failed: %s", esp_err_to_name(err));
            s_ws_state = XIAOZHI_WS_STATE_ERROR;
            return err;
        }
        s_local_audio_checked = true;
    } else if (!CONFIG_XIAOZHI_AUDIO_BOOT_LOOPBACK_TEST) {
        s_local_audio_checked = true;
    }

    const esp_websocket_client_config_t websocket_cfg = {
        .uri = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .network_timeout_ms = 10000,
        .reconnect_timeout_ms = 5000,
        .buffer_size = 2048,
        .task_stack = 6144,
        .task_prio = 6,
    };

    s_ws_client = esp_websocket_client_init(&websocket_cfg);
    if (s_ws_client == NULL) {
        s_ws_state = XIAOZHI_WS_STATE_ERROR;
        return ESP_ERR_NO_MEM;
    }

    err = esp_websocket_client_append_header(s_ws_client, "Authorization", token);
    if (err == ESP_OK) {
        err = esp_websocket_client_append_header(s_ws_client, "Protocol-Version", "1");
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "configure websocket headers failed: %s", esp_err_to_name(err));
        esp_websocket_client_destroy(s_ws_client);
        s_ws_client = NULL;
        s_ws_state = XIAOZHI_WS_STATE_ERROR;
        return err;
    }

    esp_websocket_register_events(s_ws_client, WEBSOCKET_EVENT_ANY, websocket_event_handler, NULL);

    s_ws_state = XIAOZHI_WS_STATE_CONNECTING;
    ESP_LOGI(TAG, "websocket connecting, url=%s", url);
    log_token_summary(token);

    err = esp_websocket_client_start(s_ws_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start websocket client failed: %s", esp_err_to_name(err));
        esp_websocket_unregister_events(s_ws_client, WEBSOCKET_EVENT_ANY, websocket_event_handler);
        esp_websocket_client_destroy(s_ws_client);
        s_ws_client = NULL;
        s_ws_state = XIAOZHI_WS_STATE_ERROR;
        return err;
    }

    return ESP_OK;
}

esp_err_t xiaozhi_ws_stop(void)
{
    if (s_ws_state == XIAOZHI_WS_STATE_IDLE || s_ws_state == XIAOZHI_WS_STATE_DISCONNECTED) {
        return ESP_OK;
    }

    audio_pcm_service_stop_stream();
    if (s_ws_client != NULL) {
        esp_websocket_client_close(s_ws_client, pdMS_TO_TICKS(1000));
        esp_websocket_client_stop(s_ws_client);
        esp_websocket_unregister_events(s_ws_client, WEBSOCKET_EVENT_ANY, websocket_event_handler);
        esp_websocket_client_destroy(s_ws_client);
        s_ws_client = NULL;
    }

    ESP_LOGI(TAG, "websocket stopped");
    s_ws_state = XIAOZHI_WS_STATE_DISCONNECTED;
    return ESP_OK;
}

xiaozhi_ws_state_t xiaozhi_ws_get_state(void)
{
    return s_ws_state;
}
