#include "xiaozhi_ws.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "audio_opus_stream.h"
#include "cJSON.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "xiaozhi_device.h"
#include "xiaozhi_handle.h"
#include "xiaozhi_protocol.h"

static const char *TAG = "xiaozhi_ws";

static xiaozhi_ws_state_t s_ws_state = XIAOZHI_WS_STATE_IDLE;
static esp_websocket_client_handle_t s_ws_client;
static char s_session_id[XIAOZHI_PROTOCOL_SESSION_ID_MAX_LEN];
static xiaozhi_protocol_audio_params_t s_server_audio;

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
static esp_err_t wait_for_ready(uint32_t timeout_ms);
static void cleanup_websocket_client(void);

#define XIAOZHI_WS_READY_TIMEOUT_MS 10000
#define XIAOZHI_WS_READY_POLL_MS 100

static const char *state_name(xiaozhi_ws_state_t state)
{
    switch (state) {
    case XIAOZHI_WS_STATE_IDLE:
        return "IDLE";
    case XIAOZHI_WS_STATE_CONNECTING:
        return "CONNECTING";
    case XIAOZHI_WS_STATE_CONNECTED:
        return "CONNECTED";
    case XIAOZHI_WS_STATE_HELLO_SENT:
        return "HELLO_SENT";
    case XIAOZHI_WS_STATE_READY:
        return "READY";
    case XIAOZHI_WS_STATE_LISTENING:
        return "LISTENING";
    case XIAOZHI_WS_STATE_SPEAKING:
        return "SPEAKING";
    case XIAOZHI_WS_STATE_DISCONNECTED:
        return "DISCONNECTED";
    case XIAOZHI_WS_STATE_ERROR:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}

static void set_state(xiaozhi_ws_state_t next)
{
    if (s_ws_state == next) {
        return;
    }

    ESP_LOGI(TAG, "state transition %s -> %s", state_name(s_ws_state), state_name(next));
    s_ws_state = next;
}

static void log_heap_stats(const char *label)
{
    ESP_LOGI(TAG,
             "%s free heap=%u internal free=%u minimum free heap=%u",
             label,
             (unsigned int)esp_get_free_heap_size(),
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned int)esp_get_minimum_free_heap_size());
}

static void log_token_summary(const char *token)
{
    if (token == NULL) {
        ESP_LOGI(TAG, "websocket token: <null>");
        return;
    }

    size_t len = strlen(token);
    ESP_LOGI(TAG, "websocket token present, len=%u", (unsigned int)len);
}

static void log_error_if_nonzero(const char *message, int error_code)
{
    if (error_code != 0) {
        ESP_LOGE(TAG, "%s: 0x%x", message, error_code);
    }
}

static const char *listen_mode_name(xiaozhi_ws_listen_mode_t mode)
{
    switch (mode) {
    case XIAOZHI_WS_LISTEN_MODE_BUTTON:
        return "manual";
    case XIAOZHI_WS_LISTEN_MODE_WAKE:
        return "auto";
    case XIAOZHI_WS_LISTEN_MODE_AUTO:
    default:
        return "auto";
    }
}

static bool is_bearer_token(const char *token)
{
    return token != NULL && strncmp(token, "Bearer ", 7) == 0;
}

static esp_err_t append_authorization_header(esp_websocket_client_handle_t client, const char *token)
{
    ESP_RETURN_ON_FALSE(client != NULL && token != NULL && token[0] != '\0', ESP_ERR_INVALID_ARG, TAG, "invalid auth header args");

    if (is_bearer_token(token)) {
        return esp_websocket_client_append_header(client, "Authorization", token);
    }

    char header_value[512];
    int written = snprintf(header_value, sizeof(header_value), "Bearer %s", token);
    ESP_RETURN_ON_FALSE(written > 0 && (size_t)written < sizeof(header_value), ESP_ERR_INVALID_SIZE, TAG, "token too long");
    return esp_websocket_client_append_header(client, "Authorization", header_value);
}

static esp_err_t configure_headers(esp_websocket_client_handle_t client)
{
    char mac[XIAOZHI_MAC_STR_LEN] = {0};
    char uuid[XIAOZHI_UUID_STR_LEN] = {0};
    const char *token = xiaozhi_handle_get_websocket_token();

    ESP_RETURN_ON_ERROR(xiaozhi_device_get_mac_str(mac, sizeof(mac)), TAG, "get Device-Id failed");
    ESP_RETURN_ON_ERROR(xiaozhi_device_get_or_create_uuid(uuid, sizeof(uuid)), TAG, "get Client-Id failed");
    ESP_RETURN_ON_ERROR(append_authorization_header(client, token), TAG, "append Authorization failed");
    ESP_RETURN_ON_ERROR(esp_websocket_client_append_header(client, "Protocol-Version", "1"), TAG, "append Protocol-Version failed");
    ESP_RETURN_ON_ERROR(esp_websocket_client_append_header(client, "Device-Id", mac), TAG, "append Device-Id failed");
    ESP_RETURN_ON_ERROR(esp_websocket_client_append_header(client, "Client-Id", uuid), TAG, "append Client-Id failed");

    ESP_LOGI(TAG, "websocket headers configured Device-Id=%s Client-Id=%s", mac, uuid);
    return ESP_OK;
}

static esp_err_t send_text_json(char *json, const char *label)
{
    if (json == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ws_client == NULL || !esp_websocket_client_is_connected(s_ws_client)) {
        cJSON_free(json);
        return ESP_ERR_INVALID_STATE;
    }

    int len = (int)strlen(json);
    int sent = esp_websocket_client_send_text(s_ws_client, json, len, pdMS_TO_TICKS(1000));
    if (sent != len) {
        ESP_LOGE(TAG, "%s send failed sent=%d len=%d", label, sent, len);
        cJSON_free(json);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "%s sent", label);
    cJSON_free(json);
    return ESP_OK;
}

static esp_err_t send_hello(void)
{
    char *json = NULL;
    ESP_RETURN_ON_ERROR(xiaozhi_protocol_build_hello_json(&json), TAG, "build hello failed");
    esp_err_t err = send_text_json(json, "hello");
    if (err == ESP_OK) {
        set_state(XIAOZHI_WS_STATE_HELLO_SENT);
        log_heap_stats("hello sent");
    }
    return err;
}

static esp_err_t send_opus_frame(const uint8_t *opus, size_t len, void *user_ctx)
{
    (void)user_ctx;
    if (s_ws_client == NULL || !esp_websocket_client_is_connected(s_ws_client) || s_ws_state != XIAOZHI_WS_STATE_LISTENING) {
        return ESP_ERR_INVALID_STATE;
    }

    int sent = esp_websocket_client_send_bin(s_ws_client, (const char *)opus, (int)len, pdMS_TO_TICKS(200));
    return sent == (int)len ? ESP_OK : ESP_FAIL;
}

static esp_err_t start_audio_stream(void)
{
    const audio_opus_stream_config_t config = {
        .send_cb = send_opus_frame,
        .user_ctx = NULL,
        .output_volume = -1,
    };
    return audio_opus_stream_start(&config);
}

static void stop_audio_stream(void)
{
    (void)audio_opus_stream_stop();
}

static void clear_session_state(void)
{
    memset(s_session_id, 0, sizeof(s_session_id));
    memset(&s_server_audio, 0, sizeof(s_server_audio));
}

static void cleanup_websocket_client(void)
{
    if (s_ws_client == NULL) {
        return;
    }

    if (esp_websocket_client_is_connected(s_ws_client)) {
        (void)esp_websocket_client_close(s_ws_client, pdMS_TO_TICKS(1000));
    }
    (void)esp_websocket_client_stop(s_ws_client);
    (void)esp_websocket_unregister_events(s_ws_client, WEBSOCKET_EVENT_ANY, websocket_event_handler);
    (void)esp_websocket_client_destroy(s_ws_client);
    s_ws_client = NULL;
    clear_session_state();
}

static void handle_server_hello(const xiaozhi_protocol_msg_t *msg)
{
    if (msg->transport[0] != '\0' && strcmp(msg->transport, XIAOZHI_PROTOCOL_TRANSPORT) != 0) {
        ESP_LOGE(TAG, "server hello transport mismatch: %s", msg->transport);
        set_state(XIAOZHI_WS_STATE_ERROR);
        return;
    }

    strlcpy(s_session_id, msg->session_id, sizeof(s_session_id));
    s_server_audio = msg->audio;

    ESP_LOGI(TAG, "server hello received");
    ESP_LOGI(TAG, "session_id=%s", s_session_id[0] != '\0' ? s_session_id : "<missing>");
    ESP_LOGI(TAG,
             "server audio params format=%s sample_rate=%d channels=%d frame_duration=%d",
             s_server_audio.format,
             s_server_audio.sample_rate,
             s_server_audio.channels,
             s_server_audio.frame_duration_ms);

    set_state(XIAOZHI_WS_STATE_READY);
}

static void handle_tts(const xiaozhi_protocol_msg_t *msg)
{
    if (strcmp(msg->state, "start") == 0) {
        ESP_LOGI(TAG, "tts start");
        (void)audio_opus_stream_set_uplink_enabled(false);
        set_state(XIAOZHI_WS_STATE_SPEAKING);
        return;
    }

    if (strcmp(msg->state, "stop") == 0) {
        ESP_LOGI(TAG, "tts stop");
        (void)audio_opus_stream_set_uplink_enabled(false);
        set_state(XIAOZHI_WS_STATE_READY);
        return;
    }

    ESP_LOGI(TAG, "tts state=%s", msg->state[0] != '\0' ? msg->state : "<empty>");
}

static void handle_server_message(const char *json, size_t len)
{
    xiaozhi_protocol_msg_t msg = {0};
    esp_err_t err = xiaozhi_protocol_parse_server_message(json, len, &msg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "parse server message failed: %s", esp_err_to_name(err));
        return;
    }

    switch (msg.type) {
    case XIAOZHI_PROTOCOL_MSG_HELLO:
        handle_server_hello(&msg);
        break;
    case XIAOZHI_PROTOCOL_MSG_STT:
        ESP_LOGI(TAG, "stt text=%s", msg.text[0] != '\0' ? msg.text : "<empty>");
        break;
    case XIAOZHI_PROTOCOL_MSG_TTS:
        handle_tts(&msg);
        break;
    case XIAOZHI_PROTOCOL_MSG_LLM:
        ESP_LOGI(TAG, "llm text=%s", msg.text[0] != '\0' ? msg.text : "<empty>");
        break;
    case XIAOZHI_PROTOCOL_MSG_MCP:
        ESP_LOGI(TAG, "mcp message received");
        break;
    case XIAOZHI_PROTOCOL_MSG_SYSTEM:
        ESP_LOGI(TAG, "system message state=%s text=%s", msg.state, msg.text);
        break;
    case XIAOZHI_PROTOCOL_MSG_ALERT:
        ESP_LOGW(TAG, "alert text=%s", msg.text[0] != '\0' ? msg.text : "<empty>");
        break;
    case XIAOZHI_PROTOCOL_MSG_UNKNOWN:
    default:
        ESP_LOGI(TAG, "unknown server message len=%u", (unsigned int)len);
        break;
    }
}

static void handle_binary_opus(const uint8_t *data, size_t len)
{
    ESP_LOGI(TAG, "binary opus received len=%u", (unsigned int)len);
    if (s_ws_state == XIAOZHI_WS_STATE_LISTENING) {
        (void)audio_opus_stream_set_uplink_enabled(false);
        set_state(XIAOZHI_WS_STATE_SPEAKING);
    }

    esp_err_t err = audio_opus_stream_enqueue_downlink_opus(data, len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "enqueue downlink opus failed: %s", esp_err_to_name(err));
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
        set_state(XIAOZHI_WS_STATE_CONNECTED);
        if (send_hello() != ESP_OK) {
            set_state(XIAOZHI_WS_STATE_ERROR);
        }
        break;
    case WEBSOCKET_EVENT_DATA:
        if (data->data_ptr == NULL || data->data_len <= 0) {
            break;
        }
        if (data->op_code == 0x2) {
            handle_binary_opus((const uint8_t *)data->data_ptr, (size_t)data->data_len);
        } else {
            handle_server_message(data->data_ptr, (size_t)data->data_len);
        }
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "websocket disconnected");
        log_error_if_nonzero("HTTP status", data->error_handle.esp_ws_handshake_status_code);
        stop_audio_stream();
        clear_session_state();
        set_state(XIAOZHI_WS_STATE_DISCONNECTED);
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "websocket error");
        log_error_if_nonzero("HTTP status", data->error_handle.esp_ws_handshake_status_code);
        if (data->error_handle.error_type == WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT) {
            log_error_if_nonzero("esp-tls", data->error_handle.esp_tls_last_esp_err);
            log_error_if_nonzero("tls stack", data->error_handle.esp_tls_stack_err);
            log_error_if_nonzero("socket errno", data->error_handle.esp_transport_sock_errno);
        }
        stop_audio_stream();
        set_state(XIAOZHI_WS_STATE_ERROR);
        break;
    case WEBSOCKET_EVENT_CLOSED:
        ESP_LOGI(TAG, "websocket closed");
        stop_audio_stream();
        set_state(XIAOZHI_WS_STATE_DISCONNECTED);
        break;
    default:
        break;
    }
}

esp_err_t xiaozhi_ws_start(void)
{
    if (!xiaozhi_handle_is_activated()) {
        ESP_LOGW(TAG, "skip websocket start because device is not activated");
        set_state(XIAOZHI_WS_STATE_ERROR);
        return ESP_ERR_INVALID_STATE;
    }

    const char *url = xiaozhi_handle_get_websocket_url();
    const char *token = xiaozhi_handle_get_websocket_token();
    if (url == NULL || url[0] == '\0' || token == NULL || token[0] == '\0') {
        ESP_LOGW(TAG, "skip websocket start because url or token is missing");
        set_state(XIAOZHI_WS_STATE_ERROR);
        return ESP_ERR_INVALID_STATE;
    }

    if (s_ws_client != NULL) {
        if (s_ws_state == XIAOZHI_WS_STATE_READY ||
            s_ws_state == XIAOZHI_WS_STATE_LISTENING ||
            s_ws_state == XIAOZHI_WS_STATE_SPEAKING ||
            s_ws_state == XIAOZHI_WS_STATE_CONNECTING ||
            s_ws_state == XIAOZHI_WS_STATE_CONNECTED ||
            s_ws_state == XIAOZHI_WS_STATE_HELLO_SENT) {
            ESP_LOGW(TAG, "websocket client already started");
            return ESP_OK;
        }

        ESP_LOGW(TAG, "cleanup stale websocket client before reconnect state=%s", state_name(s_ws_state));
        cleanup_websocket_client();
    }

    clear_session_state();

    const esp_websocket_client_config_t websocket_cfg = {
        .uri = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .network_timeout_ms = 10000,
        .reconnect_timeout_ms = 5000,
        .disable_auto_reconnect = true,
        .buffer_size = 2048,
        .task_stack = 6144,
        .task_prio = 6,
    };

    s_ws_client = esp_websocket_client_init(&websocket_cfg);
    if (s_ws_client == NULL) {
        set_state(XIAOZHI_WS_STATE_ERROR);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = configure_headers(s_ws_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "configure websocket headers failed: %s", esp_err_to_name(err));
        esp_websocket_client_destroy(s_ws_client);
        s_ws_client = NULL;
        set_state(XIAOZHI_WS_STATE_ERROR);
        return err;
    }

    err = esp_websocket_register_events(s_ws_client, WEBSOCKET_EVENT_ANY, websocket_event_handler, NULL);
    if (err != ESP_OK) {
        esp_websocket_client_destroy(s_ws_client);
        s_ws_client = NULL;
        set_state(XIAOZHI_WS_STATE_ERROR);
        return err;
    }

    set_state(XIAOZHI_WS_STATE_CONNECTING);
    ESP_LOGI(TAG, "websocket connecting, url=%s", url);
    log_token_summary(token);
    log_heap_stats("before websocket start");

    err = esp_websocket_client_start(s_ws_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start websocket client failed: %s", esp_err_to_name(err));
        esp_websocket_unregister_events(s_ws_client, WEBSOCKET_EVENT_ANY, websocket_event_handler);
        esp_websocket_client_destroy(s_ws_client);
        s_ws_client = NULL;
        set_state(XIAOZHI_WS_STATE_ERROR);
        return err;
    }

    return ESP_OK;
}

esp_err_t xiaozhi_ws_stop(void)
{
    if ((s_ws_state == XIAOZHI_WS_STATE_IDLE || s_ws_state == XIAOZHI_WS_STATE_DISCONNECTED) && s_ws_client == NULL) {
        return ESP_OK;
    }

    stop_audio_stream();
    cleanup_websocket_client();

    ESP_LOGI(TAG, "websocket stopped");
    set_state(XIAOZHI_WS_STATE_DISCONNECTED);
    return ESP_OK;
}

xiaozhi_ws_state_t xiaozhi_ws_get_state(void)
{
    return s_ws_state;
}

bool xiaozhi_ws_is_ready(void)
{
    return s_ws_state == XIAOZHI_WS_STATE_READY || s_ws_state == XIAOZHI_WS_STATE_LISTENING;
}

static esp_err_t wait_for_ready(uint32_t timeout_ms)
{
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    while (xTaskGetTickCount() < deadline) {
        if (xiaozhi_ws_is_ready()) {
            return ESP_OK;
        }
        if (s_ws_state == XIAOZHI_WS_STATE_ERROR || s_ws_state == XIAOZHI_WS_STATE_DISCONNECTED) {
            ESP_LOGW(TAG, "websocket failed while waiting for READY state=%s", state_name(s_ws_state));
            return ESP_FAIL;
        }
        vTaskDelay(pdMS_TO_TICKS(XIAOZHI_WS_READY_POLL_MS));
    }

    ESP_LOGW(TAG, "websocket READY wait timeout after %u ms", (unsigned int)timeout_ms);
    return ESP_ERR_TIMEOUT;
}

esp_err_t xiaozhi_ws_trigger_listen(xiaozhi_ws_listen_mode_t mode)
{
    if (s_ws_state == XIAOZHI_WS_STATE_LISTENING) {
        return ESP_OK;
    }
    if (s_ws_state == XIAOZHI_WS_STATE_IDLE || s_ws_state == XIAOZHI_WS_STATE_DISCONNECTED || s_ws_state == XIAOZHI_WS_STATE_ERROR) {
        esp_err_t err = xiaozhi_ws_start();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "open websocket for listen failed: %s", esp_err_to_name(err));
            return err;
        }
    }
    if (s_ws_state == XIAOZHI_WS_STATE_CONNECTING || s_ws_state == XIAOZHI_WS_STATE_CONNECTED || s_ws_state == XIAOZHI_WS_STATE_HELLO_SENT) {
        esp_err_t err = wait_for_ready(XIAOZHI_WS_READY_TIMEOUT_MS);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "listen start blocked waiting websocket READY: %s", esp_err_to_name(err));
            return err;
        }
    }
    if (s_ws_state != XIAOZHI_WS_STATE_READY) {
        ESP_LOGW(TAG, "ignore listen start in state=%s", state_name(s_ws_state));
        return ESP_ERR_INVALID_STATE;
    }
    if (s_session_id[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = start_audio_stream();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start opus stream failed before listen start: %s", esp_err_to_name(err));
        set_state(XIAOZHI_WS_STATE_ERROR);
        (void)xiaozhi_ws_stop();
        return err;
    }

    char *json = NULL;
    ESP_RETURN_ON_ERROR(xiaozhi_protocol_build_listen_start_json(s_session_id, listen_mode_name(mode), &json), TAG, "build listen start failed");
    err = send_text_json(json, "listen start");
    if (err != ESP_OK) {
        stop_audio_stream();
        return err;
    }

    (void)audio_opus_stream_set_uplink_enabled(true);
    set_state(XIAOZHI_WS_STATE_LISTENING);
    return ESP_OK;
}

esp_err_t xiaozhi_ws_stop_listen(void)
{
    if (s_ws_state != XIAOZHI_WS_STATE_LISTENING) {
        return ESP_OK;
    }

    (void)audio_opus_stream_set_uplink_enabled(false);
    char *json = NULL;
    ESP_RETURN_ON_ERROR(xiaozhi_protocol_build_listen_stop_json(s_session_id, &json), TAG, "build listen stop failed");
    esp_err_t err = send_text_json(json, "listen stop");
    set_state(err == ESP_OK ? XIAOZHI_WS_STATE_READY : XIAOZHI_WS_STATE_ERROR);
    return err;
}

esp_err_t xiaozhi_ws_abort_listening(const char *reason)
{
    if (s_session_id[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    (void)audio_opus_stream_set_uplink_enabled(false);
    char *json = NULL;
    ESP_RETURN_ON_ERROR(xiaozhi_protocol_build_abort_json(s_session_id, reason, &json), TAG, "build abort failed");
    esp_err_t err = send_text_json(json, "abort");
    set_state(err == ESP_OK ? XIAOZHI_WS_STATE_READY : XIAOZHI_WS_STATE_ERROR);
    return err;
}

esp_err_t xiaozhi_ws_notify_server_hello(const char *json, size_t len)
{
    handle_server_message(json, len);
    return s_ws_state == XIAOZHI_WS_STATE_READY ? ESP_OK : ESP_FAIL;
}

esp_err_t xiaozhi_ws_notify_binary_opus(const uint8_t *data, size_t len)
{
    handle_binary_opus(data, len);
    return ESP_OK;
}

esp_err_t xiaozhi_ws_notify_text(const char *text)
{
    if (text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    handle_server_message(text, strlen(text));
    return ESP_OK;
}
