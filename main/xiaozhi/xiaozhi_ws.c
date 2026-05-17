#include "xiaozhi_ws.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "audio_opus_codec.h"
#include "audio_opus_stream.h"
#include "bsp_audio.h"
#include "cJSON.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "xiaozhi_device.h"
#include "xiaozhi_handle.h"
#include "xiaozhi_protocol.h"

static const char *TAG = "xiaozhi_ws";

static xiaozhi_ws_state_t s_ws_state = XIAOZHI_WS_STATE_DISCONNECTED;
static esp_websocket_client_handle_t s_ws_client;
static char s_session_id[XIAOZHI_PROTOCOL_SESSION_ID_MAX_LEN];
static xiaozhi_protocol_audio_params_t s_server_audio;
static TickType_t s_next_opus_send_tick;
static bool s_reconnect_in_progress;
static bool s_waiting_tts_stop;
static uint32_t s_binary_opus_diagnostics_frames;

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
static esp_err_t wait_for_ready(uint32_t timeout_ms);
static void cleanup_websocket_client(void);
static void handle_websocket_write_failure(const char *stage, int sent, size_t expected_len);
static void stop_opus_audio_stream(void);
static esp_err_t ensure_websocket_ready(void);
static esp_err_t send_listen_state(const char *state, const char *mode);
static esp_err_t start_audio_stream_with_rate(audio_opus_pcm_source_t pcm_source, int decoder_output_sample_rate);
static esp_err_t restore_downlink_audio_stream(audio_opus_pcm_source_t pcm_source);
static void reset_session_flags(void);

#define XIAOZHI_WS_READY_TIMEOUT_MS 10000
#define XIAOZHI_WS_READY_POLL_MS 100
#define XIAOZHI_WS_OPUS_SEND_INTERVAL_MS XIAOZHI_PROTOCOL_AUDIO_FRAME_DURATION_MS
#define XIAOZHI_WS_OPUS_SEND_TIMEOUT_MS 1000
#define XIAOZHI_WS_DOWNLINK_DRAIN_TIMEOUT_MS 1200
#define XIAOZHI_WS_BINARY_OPUS_DIAGNOSTIC_INTERVAL 16

static const char *state_name(xiaozhi_ws_state_t state)
{
    switch (state) {
    case XIAOZHI_WS_STATE_DISCONNECTED:
        return "DISCONNECTED";
    case XIAOZHI_WS_STATE_CONNECTING:
        return "CONNECTING";
    case XIAOZHI_WS_STATE_WS_CONNECTED:
        return "WS_CONNECTED";
    case XIAOZHI_WS_STATE_HELLO_SENT:
        return "HELLO_SENT";
    case XIAOZHI_WS_STATE_READY:
        return "READY";
    case XIAOZHI_WS_STATE_WAKE_DETECTED:
        return "WAKE_DETECTED";
    case XIAOZHI_WS_STATE_LISTENING:
        return "LISTENING";
    case XIAOZHI_WS_STATE_WAITING_RESPONSE:
        return "WAITING_RESPONSE";
    case XIAOZHI_WS_STATE_SPEAKING:
        return "SPEAKING";
    case XIAOZHI_WS_STATE_CLOSING:
        return "CLOSING";
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

static void reset_session_flags(void)
{
    s_waiting_tts_stop = false;
    s_next_opus_send_tick = 0;
    s_binary_opus_diagnostics_frames = 0;
}

static bool should_log_binary_opus_diagnostics(void)
{
    uint32_t frame = ++s_binary_opus_diagnostics_frames;
    return frame == 1 || (frame % XIAOZHI_WS_BINARY_OPUS_DIAGNOSTIC_INTERVAL) == 0;
}

static bool is_ready_or_busy_state(xiaozhi_ws_state_t state)
{
    return state == XIAOZHI_WS_STATE_READY ||
           state == XIAOZHI_WS_STATE_WAKE_DETECTED ||
           state == XIAOZHI_WS_STATE_LISTENING ||
           state == XIAOZHI_WS_STATE_WAITING_RESPONSE ||
           state == XIAOZHI_WS_STATE_SPEAKING;
}

static bool can_send_business_message(void)
{
    return s_ws_client != NULL &&
           esp_websocket_client_is_connected(s_ws_client) &&
           (s_ws_state == XIAOZHI_WS_STATE_READY ||
            s_ws_state == XIAOZHI_WS_STATE_WAKE_DETECTED ||
            s_ws_state == XIAOZHI_WS_STATE_WAITING_RESPONSE);
}

static void log_heap_stats(const char *label)
{
    ESP_LOGI(TAG,
             "%s heap: internal_free=%u internal_largest=%u 8bit_free=%u 8bit_largest=%u spiram_free=%u spiram_largest=%u minimum_free_heap=%u",
             label,
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
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
    if (label != NULL && strncmp(label, "listen", 6) == 0) {
        ESP_LOGI(TAG, "%s payload=%s", label, json);
    }
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
    if (s_ws_client == NULL || !esp_websocket_client_is_connected(s_ws_client) || s_session_id[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_ws_state != XIAOZHI_WS_STATE_LISTENING) {
        ESP_LOGW(TAG, "drop opus frame before listening state=%s len=%u", state_name(s_ws_state), (unsigned int)len);
        return ESP_ERR_INVALID_STATE;
    }

    TickType_t now = xTaskGetTickCount();
    if (s_next_opus_send_tick != 0 && now < s_next_opus_send_tick) {
        vTaskDelay(s_next_opus_send_tick - now);
    }

    int sent = esp_websocket_client_send_bin(s_ws_client, (const char *)opus, (int)len, pdMS_TO_TICKS(XIAOZHI_WS_OPUS_SEND_TIMEOUT_MS));
    if (sent != (int)len) {
        handle_websocket_write_failure("opus send", sent, len);
        return ESP_FAIL;
    }

    s_next_opus_send_tick = xTaskGetTickCount() + pdMS_TO_TICKS(XIAOZHI_WS_OPUS_SEND_INTERVAL_MS);
    return ESP_OK;
}

static int resolve_decoder_output_sample_rate(void)
{
    if (s_server_audio.format[0] != '\0' && strcmp(s_server_audio.format, XIAOZHI_PROTOCOL_AUDIO_FORMAT) != 0) {
        ESP_LOGW(TAG, "unsupported server audio format=%s, fallback sample_rate=%d", s_server_audio.format, AUDIO_OPUS_SAMPLE_RATE);
        return AUDIO_OPUS_SAMPLE_RATE;
    }
    if (s_server_audio.channels > 0 && s_server_audio.channels != AUDIO_OPUS_CHANNELS) {
        ESP_LOGW(TAG, "unsupported server audio channels=%d, fallback sample_rate=%d", s_server_audio.channels, AUDIO_OPUS_SAMPLE_RATE);
        return AUDIO_OPUS_SAMPLE_RATE;
    }
    if (s_server_audio.frame_duration_ms > 0 && s_server_audio.frame_duration_ms != AUDIO_OPUS_FRAME_DURATION_MS) {
        ESP_LOGW(TAG, "unsupported server frame_duration=%d, fallback sample_rate=%d", s_server_audio.frame_duration_ms, AUDIO_OPUS_SAMPLE_RATE);
        return AUDIO_OPUS_SAMPLE_RATE;
    }
    if (s_server_audio.sample_rate == 16000 || s_server_audio.sample_rate == 24000) {
        return s_server_audio.sample_rate;
    }
    ESP_LOGW(TAG, "server sample_rate=%d unsupported, fallback sample_rate=%d", s_server_audio.sample_rate, AUDIO_OPUS_SAMPLE_RATE);
    return AUDIO_OPUS_SAMPLE_RATE;
}

static esp_err_t start_audio_stream(audio_opus_pcm_source_t pcm_source)
{
    const audio_opus_stream_config_t config = {
        .send_cb = send_opus_frame,
        .user_ctx = NULL,
        .output_volume = -1,
        .pcm_source = pcm_source,
        .decoder_output_sample_rate = resolve_decoder_output_sample_rate(),
    };
    esp_err_t err = audio_opus_stream_start(&config);
    return err;
}

static esp_err_t start_audio_stream_with_rate(audio_opus_pcm_source_t pcm_source, int decoder_output_sample_rate)
{
    const audio_opus_stream_config_t config = {
        .send_cb = send_opus_frame,
        .user_ctx = NULL,
        .output_volume = -1,
        .pcm_source = pcm_source,
        .decoder_output_sample_rate = decoder_output_sample_rate,
    };
    return audio_opus_stream_start(&config);
}

static esp_err_t restore_downlink_audio_stream(audio_opus_pcm_source_t pcm_source)
{
    ESP_RETURN_ON_FALSE(pcm_source == AUDIO_OPUS_PCM_SOURCE_EXTERNAL_FEED,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "invalid downlink restore pcm_source=%d",
                        pcm_source);
    const int playback_sample_rate = resolve_decoder_output_sample_rate();
    ESP_LOGI(TAG,
             "manual listen restore audio path current_sample_rate=%d target_sample_rate=%d",
             bsp_audio_get_current_sample_rate(),
             playback_sample_rate);

    stop_opus_audio_stream();
    ESP_RETURN_ON_ERROR(bsp_audio_open_with_sample_rate(playback_sample_rate), TAG, "restore downlink audio path failed");

    esp_err_t err = start_audio_stream(pcm_source);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "restore downlink opus stream failed: %s", esp_err_to_name(err));
        return err;
    }

    (void)audio_opus_stream_set_uplink_enabled(false);
    audio_opus_stream_flush();
    return ESP_OK;
}

static void stop_session_audio_io(void)
{
    reset_session_flags();
    (void)audio_opus_stream_set_uplink_enabled(false);
    audio_opus_stream_flush();
    stop_opus_audio_stream();
}

static void stop_opus_audio_stream(void)
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
    s_next_opus_send_tick = 0;
    reset_session_flags();
}

static void handle_websocket_write_failure(const char *stage, int sent, size_t expected_len)
{
    ESP_LOGE(TAG,
             "%s failed sent=%d expected=%u state=%s connected=%d",
             stage,
             sent,
             (unsigned int)expected_len,
             state_name(s_ws_state),
             s_ws_client != NULL ? esp_websocket_client_is_connected(s_ws_client) : 0);
    log_heap_stats("websocket write failure");

    if (s_reconnect_in_progress) {
        return;
    }
    s_reconnect_in_progress = true;

    stop_session_audio_io();
    cleanup_websocket_client();
    set_state(XIAOZHI_WS_STATE_DISCONNECTED);

    s_reconnect_in_progress = false;
}

static void handle_server_hello(const xiaozhi_protocol_msg_t *msg)
{
    if (msg->transport[0] != '\0' && strcmp(msg->transport, XIAOZHI_PROTOCOL_TRANSPORT) != 0) {
        ESP_LOGE(TAG, "server hello transport mismatch: %s", msg->transport);
        set_state(XIAOZHI_WS_STATE_DISCONNECTED);
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

    esp_err_t err = start_audio_stream(AUDIO_OPUS_PCM_SOURCE_EXTERNAL_FEED);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start opus voice stream failed: %s", esp_err_to_name(err));
        set_state(XIAOZHI_WS_STATE_DISCONNECTED);
        return;
    }
    (void)audio_opus_stream_set_uplink_enabled(false);
    audio_opus_stream_flush();

    set_state(XIAOZHI_WS_STATE_READY);
    log_heap_stats("WS READY");
    audio_opus_stream_log_watermarks("WS READY");
}

static void handle_tts(const xiaozhi_protocol_msg_t *msg)
{
    if (strcmp(msg->state, "start") == 0) {
        ESP_LOGI(TAG, "tts start");
        s_waiting_tts_stop = true;
        (void)audio_opus_stream_set_uplink_enabled(false);
        set_state(XIAOZHI_WS_STATE_SPEAKING);
        log_heap_stats("TTS start");
        audio_opus_stream_log_watermarks("TTS start");
        return;
    }

    if (strcmp(msg->state, "stop") == 0) {
        ESP_LOGI(TAG, "tts stop");
        (void)audio_opus_stream_set_uplink_enabled(false);
        (void)audio_opus_stream_wait_downlink_idle(XIAOZHI_WS_DOWNLINK_DRAIN_TIMEOUT_MS);
        (void)audio_opus_stream_close_decoder();
        s_waiting_tts_stop = false;
        set_state(XIAOZHI_WS_STATE_READY);
        ESP_LOGI(TAG, "tts stop -> READY");
        log_heap_stats("TTS stop");
        audio_opus_stream_log_watermarks("TTS stop");
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
        ESP_LOGI(TAG, "tts state=%s text=%s session_id=%s",
                 msg.state[0] != '\0' ? msg.state : "<empty>",
                 msg.text[0] != '\0' ? msg.text : "<empty>",
                 msg.session_id[0] != '\0' ? msg.session_id : "<missing>");
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
    if (s_ws_state == XIAOZHI_WS_STATE_LISTENING || s_ws_state == XIAOZHI_WS_STATE_WAITING_RESPONSE) {
        s_waiting_tts_stop = true;
        (void)audio_opus_stream_set_uplink_enabled(false);
        set_state(XIAOZHI_WS_STATE_SPEAKING);
    }

    esp_err_t err = audio_opus_stream_enqueue_downlink_opus(data, len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "enqueue downlink opus failed: %s", esp_err_to_name(err));
    } else if (should_log_binary_opus_diagnostics()) {
        audio_opus_stream_log_watermarks("binary opus");
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
        set_state(XIAOZHI_WS_STATE_WS_CONNECTED);
        if (send_hello() != ESP_OK) {
            set_state(XIAOZHI_WS_STATE_DISCONNECTED);
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
        stop_session_audio_io();
        clear_session_state();
        reset_session_flags();
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
        stop_session_audio_io();
        reset_session_flags();
        clear_session_state();
        set_state(XIAOZHI_WS_STATE_DISCONNECTED);
        break;
    case WEBSOCKET_EVENT_CLOSED:
        ESP_LOGI(TAG, "websocket closed");
        stop_session_audio_io();
        reset_session_flags();
        set_state(XIAOZHI_WS_STATE_DISCONNECTED);
        break;
    default:
        break;
    }
}

esp_err_t xiaozhi_ws_start(void)
{
    log_heap_stats("xiaozhi_ws_start entry");

    if (!xiaozhi_handle_is_activated()) {
        ESP_LOGW(TAG, "skip websocket start because device is not activated");
        set_state(XIAOZHI_WS_STATE_DISCONNECTED);
        return ESP_ERR_INVALID_STATE;
    }

    const char *url = xiaozhi_handle_get_websocket_url();
    const char *token = xiaozhi_handle_get_websocket_token();
    if (url == NULL || url[0] == '\0' || token == NULL || token[0] == '\0') {
        ESP_LOGW(TAG, "skip websocket start because url or token is missing");
        set_state(XIAOZHI_WS_STATE_DISCONNECTED);
        return ESP_ERR_INVALID_STATE;
    }

    if (s_ws_client != NULL) {
        if (s_ws_state == XIAOZHI_WS_STATE_CONNECTING ||
            s_ws_state == XIAOZHI_WS_STATE_WS_CONNECTED ||
            s_ws_state == XIAOZHI_WS_STATE_HELLO_SENT ||
            is_ready_or_busy_state(s_ws_state)) {
            ESP_LOGW(TAG, "websocket client already started state=%s", state_name(s_ws_state));
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
        .task_stack = 4096,
        .task_prio = 6,
    };

    s_ws_client = esp_websocket_client_init(&websocket_cfg);
    if (s_ws_client == NULL) {
        set_state(XIAOZHI_WS_STATE_DISCONNECTED);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = configure_headers(s_ws_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "configure websocket headers failed: %s", esp_err_to_name(err));
        esp_websocket_client_destroy(s_ws_client);
        s_ws_client = NULL;
        set_state(XIAOZHI_WS_STATE_DISCONNECTED);
        return err;
    }

    err = esp_websocket_register_events(s_ws_client, WEBSOCKET_EVENT_ANY, websocket_event_handler, NULL);
    if (err != ESP_OK) {
        esp_websocket_client_destroy(s_ws_client);
        s_ws_client = NULL;
        set_state(XIAOZHI_WS_STATE_DISCONNECTED);
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
        set_state(XIAOZHI_WS_STATE_DISCONNECTED);
        return err;
    }

    return ESP_OK;
}

esp_err_t xiaozhi_ws_stop(void)
{
    if (s_ws_state == XIAOZHI_WS_STATE_DISCONNECTED && s_ws_client == NULL) {
        return ESP_OK;
    }

    set_state(XIAOZHI_WS_STATE_CLOSING);
    stop_opus_audio_stream();
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
    return is_ready_or_busy_state(s_ws_state) && s_ws_state != XIAOZHI_WS_STATE_SPEAKING;
}

static esp_err_t wait_for_ready(uint32_t timeout_ms)
{
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    while (xTaskGetTickCount() < deadline) {
        if (xiaozhi_ws_is_ready()) {
            return ESP_OK;
        }
        if (s_ws_state == XIAOZHI_WS_STATE_DISCONNECTED) {
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
    if (mode != XIAOZHI_WS_LISTEN_MODE_BUTTON) {
        esp_err_t err = xiaozhi_ws_on_wake_detected();
        return err;
    }

    if (s_ws_state == XIAOZHI_WS_STATE_LISTENING) {
        ESP_LOGI(TAG, "manual listen start ignored because already listening");
        return ESP_OK;
    }

    if (s_ws_state == XIAOZHI_WS_STATE_SPEAKING || s_waiting_tts_stop) {
        ESP_LOGW(TAG,
                 "manual listen start ignored while TTS is active state=%s waiting_tts_stop=%d",
                 state_name(s_ws_state),
                 s_waiting_tts_stop);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ensure_websocket_ready();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "manual listen start websocket not ready: %s", esp_err_to_name(err));
        return err;
    }

    if (s_session_id[0] == '\0') {
        ESP_LOGW(TAG, "manual listen start ignored because session_id is missing");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_ws_state != XIAOZHI_WS_STATE_READY && s_ws_state != XIAOZHI_WS_STATE_WAKE_DETECTED) {
        ESP_LOGW(TAG, "manual listen start invalid state=%s", state_name(s_ws_state));
        return ESP_ERR_INVALID_STATE;
    }

    (void)audio_opus_stream_set_uplink_enabled(false);
    (void)audio_opus_stream_wait_downlink_idle(XIAOZHI_WS_DOWNLINK_DRAIN_TIMEOUT_MS);
    audio_opus_stream_flush();
    stop_opus_audio_stream();

    ESP_LOGI(TAG,
             "manual listen switch audio path current_sample_rate=%d target_sample_rate=%d",
             bsp_audio_get_current_sample_rate(),
             AUDIO_OPUS_SAMPLE_RATE);
    err = bsp_audio_open_with_sample_rate(AUDIO_OPUS_SAMPLE_RATE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "switch audio path for manual capture failed: %s", esp_err_to_name(err));
        return err;
    }

    err = start_audio_stream_with_rate(AUDIO_OPUS_PCM_SOURCE_DIRECT_CODEC, AUDIO_OPUS_SAMPLE_RATE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start manual direct codec stream failed: %s", esp_err_to_name(err));
        (void)restore_downlink_audio_stream(AUDIO_OPUS_PCM_SOURCE_EXTERNAL_FEED);
        return err;
    }

    err = send_listen_state("start", "manual");
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "manual listen start send failed: %s", esp_err_to_name(err));
        (void)audio_opus_stream_set_uplink_enabled(false);
        audio_opus_stream_flush();
        (void)restore_downlink_audio_stream(AUDIO_OPUS_PCM_SOURCE_EXTERNAL_FEED);
        return err;
    }

    s_next_opus_send_tick = 0;
    set_state(XIAOZHI_WS_STATE_LISTENING);

    err = audio_opus_stream_set_uplink_enabled(true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "manual listen uplink enable failed: %s", esp_err_to_name(err));
        (void)send_listen_state("stop", "manual");
        esp_err_t restore_err = restore_downlink_audio_stream(AUDIO_OPUS_PCM_SOURCE_EXTERNAL_FEED);
        set_state(restore_err == ESP_OK ? XIAOZHI_WS_STATE_WAITING_RESPONSE : XIAOZHI_WS_STATE_DISCONNECTED);
        return err;
    }

    log_heap_stats("listen start manual");
    audio_opus_stream_log_watermarks("listen start manual");
    return ESP_OK;
}

static esp_err_t ensure_websocket_ready(void)
{
    if (is_ready_or_busy_state(s_ws_state)) {
        return ESP_OK;
    }

    if (s_ws_state == XIAOZHI_WS_STATE_DISCONNECTED) {
        ESP_RETURN_ON_ERROR(xiaozhi_ws_start(), TAG, "start websocket failed");
    }

    return wait_for_ready(XIAOZHI_WS_READY_TIMEOUT_MS);
}

static esp_err_t send_listen_state(const char *state, const char *mode)
{
    ESP_RETURN_ON_FALSE(s_session_id[0] != '\0', ESP_ERR_INVALID_STATE, TAG, "session_id missing");
    ESP_RETURN_ON_FALSE(can_send_business_message() || s_ws_state == XIAOZHI_WS_STATE_LISTENING,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "listen state=%s cannot be sent in ws state=%s",
                        state,
                        state_name(s_ws_state));

    cJSON *root = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(root != NULL, ESP_ERR_NO_MEM, TAG, "create listen root failed");

    esp_err_t err = ESP_OK;
    if (!cJSON_AddStringToObject(root, "session_id", s_session_id) ||
        !cJSON_AddStringToObject(root, "type", "listen") ||
        !cJSON_AddStringToObject(root, "state", state) ||
        !cJSON_AddStringToObject(root, "mode", (mode != NULL && mode[0] != '\0') ? mode : "manual")) {
        err = ESP_ERR_NO_MEM;
    }

    char *json = NULL;
    if (err == ESP_OK) {
        json = cJSON_PrintUnformatted(root);
        if (json == NULL) {
            err = ESP_ERR_NO_MEM;
        }
    }
    cJSON_Delete(root);
    if (err != ESP_OK) {
        return err;
    }

    err = send_text_json(json, "listen");
    if (err == ESP_OK && strcmp(state, "start") == 0) {
        ESP_LOGI(TAG, "listen start mode=%s", (mode != NULL && mode[0] != '\0') ? mode : "manual");
    }
    return err;
}

esp_err_t xiaozhi_ws_on_wake_detected(void)
{
    if (s_ws_state == XIAOZHI_WS_STATE_SPEAKING) {
        return ESP_OK;
    }

    esp_err_t err = ensure_websocket_ready();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wake detected but websocket not ready: %s", esp_err_to_name(err));
        return err;
    }

    if (s_ws_state == XIAOZHI_WS_STATE_READY) {
        set_state(XIAOZHI_WS_STATE_WAKE_DETECTED);
    }
    return ESP_OK;
}

esp_err_t xiaozhi_ws_on_vad_state(bool speech)
{
    if (s_ws_state == XIAOZHI_WS_STATE_SPEAKING || s_waiting_tts_stop) {
        return ESP_OK;
    }

    if (speech) {
        if (s_ws_state == XIAOZHI_WS_STATE_READY) {
            set_state(XIAOZHI_WS_STATE_WAKE_DETECTED);
        }
        if (s_ws_state != XIAOZHI_WS_STATE_WAKE_DETECTED && s_ws_state != XIAOZHI_WS_STATE_READY) {
            return ESP_OK;
        }

        esp_err_t err = ensure_websocket_ready();
        if (err != ESP_OK) {
            return err;
        }

        if (s_session_id[0] == '\0') {
            return ESP_ERR_INVALID_STATE;
        }

        err = send_listen_state("start", "manual");
        if (err != ESP_OK) {
            return err;
        }
        (void)audio_opus_stream_set_uplink_enabled(true);
        set_state(XIAOZHI_WS_STATE_LISTENING);
        ESP_LOGI(TAG, "vad speech -> listen start");
    } else if (s_ws_state == XIAOZHI_WS_STATE_LISTENING) {
        ESP_LOGI(TAG, "vad silence -> listen stop");
        (void)audio_opus_stream_set_uplink_enabled(false);
        (void)xiaozhi_ws_stop_listen();
        set_state(XIAOZHI_WS_STATE_WAITING_RESPONSE);
    }
    return ESP_OK;
}

esp_err_t xiaozhi_ws_feed_processed_pcm(const uint8_t *data, size_t len)
{
    if (s_ws_state != XIAOZHI_WS_STATE_LISTENING || s_ws_client == NULL || !esp_websocket_client_is_connected(s_ws_client)) {
        return ESP_OK;
    }
    return audio_opus_stream_feed_pcm(data, len);
}

esp_err_t xiaozhi_ws_trigger_detect_text(const char *text)
{
    ESP_RETURN_ON_FALSE(text != NULL && text[0] != '\0', ESP_ERR_INVALID_ARG, TAG, "detect text is empty");

    if (s_ws_state == XIAOZHI_WS_STATE_SPEAKING || s_waiting_tts_stop) {
        ESP_LOGW(TAG, "detect text ignored while speaking state=%s", state_name(s_ws_state));
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ensure_websocket_ready();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "detect text websocket not ready: %s", esp_err_to_name(err));
        return err;
    }

    if (!can_send_business_message() ||
        (s_ws_state != XIAOZHI_WS_STATE_READY && s_ws_state != XIAOZHI_WS_STATE_WAKE_DETECTED)) {
        ESP_LOGW(TAG, "detect text invalid state=%s connected=%d",
                 state_name(s_ws_state),
                 s_ws_client != NULL ? esp_websocket_client_is_connected(s_ws_client) : 0);
        return ESP_ERR_INVALID_STATE;
    }

    (void)audio_opus_stream_set_uplink_enabled(false);
    audio_opus_stream_flush();

    char *json = NULL;
    ESP_RETURN_ON_ERROR(xiaozhi_protocol_build_listen_detect_json(s_session_id, text, &json), TAG, "build listen detect failed");

    err = send_text_json(json, "listen detect");
    if (err == ESP_OK) {
        set_state(XIAOZHI_WS_STATE_WAITING_RESPONSE);
        log_heap_stats("listen detect sent");
    } else {
        set_state(XIAOZHI_WS_STATE_DISCONNECTED);
    }
    return err;
}

esp_err_t xiaozhi_ws_stop_listen(void)
{
    if (s_ws_state != XIAOZHI_WS_STATE_LISTENING && s_ws_state != XIAOZHI_WS_STATE_WAKE_DETECTED) {
        return ESP_OK;
    }

    (void)audio_opus_stream_set_uplink_enabled(false);
    audio_opus_stream_flush();
    char *json = NULL;
    ESP_RETURN_ON_ERROR(xiaozhi_protocol_build_listen_stop_json(s_session_id, &json), TAG, "build listen stop failed");
    esp_err_t err = send_text_json(json, "listen stop");
    const audio_opus_pcm_source_t restore_source = AUDIO_OPUS_PCM_SOURCE_EXTERNAL_FEED;
    esp_err_t restore_err = restore_downlink_audio_stream(restore_source);
    if (err == ESP_OK && restore_err == ESP_OK) {
        set_state(XIAOZHI_WS_STATE_WAITING_RESPONSE);
        log_heap_stats("listen stop manual");
        audio_opus_stream_log_watermarks("listen stop manual");
        return ESP_OK;
    }

    if (restore_err != ESP_OK) {
        ESP_LOGE(TAG, "restore downlink after listen stop failed: %s", esp_err_to_name(restore_err));
    }
    set_state(XIAOZHI_WS_STATE_DISCONNECTED);
    return err != ESP_OK ? err : restore_err;
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
    set_state(err == ESP_OK ? XIAOZHI_WS_STATE_READY : XIAOZHI_WS_STATE_DISCONNECTED);
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
