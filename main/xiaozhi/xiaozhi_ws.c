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
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "sdkconfig.h"
#include "xiaozhi_device.h"
#include "xiaozhi_handle.h"
#include "xiaozhi_protocol.h"
#include "xiaozhi_sr.h"

static const char *TAG = "xiaozhi_ws";

static xiaozhi_ws_state_t s_ws_state = XIAOZHI_WS_STATE_DISCONNECTED;
static esp_websocket_client_handle_t s_ws_client;
static char s_session_id[XIAOZHI_PROTOCOL_SESSION_ID_MAX_LEN];
static xiaozhi_protocol_audio_params_t s_server_audio;
static TickType_t s_next_opus_send_tick;
static bool s_reconnect_in_progress;
static bool s_waiting_tts_stop;
static bool s_pending_ptt;
static bool s_button_down;
static uint32_t s_binary_opus_diagnostics_frames;
static TimerHandle_t s_waiting_response_timer;
static TimerHandle_t s_speaking_timeout_timer;
static TimerHandle_t s_auto_silence_timer;
static TimerHandle_t s_auto_max_listen_timer;
static TimerHandle_t s_tts_resume_timer;
static QueueHandle_t s_session_event_queue;
static TaskHandle_t s_session_task_handle;
static xiaozhi_ws_state_t s_waiting_response_last_state = XIAOZHI_WS_STATE_DISCONNECTED;
static audio_opus_stream_stats_t s_waiting_response_stats;
static TickType_t s_manual_listen_start_tick;
static int64_t s_listen_start_time_us;
static int64_t s_vad_silence_start_time_us;
static xiaozhi_ws_listen_mode_t s_active_listen_mode = XIAOZHI_WS_LISTEN_MODE_AUTO;
static uint32_t s_waiting_response_timeout_ms;
static bool s_vad_muted_by_playback;

#define XIAOZHI_WS_LISTEN_MODE_MANUAL XIAOZHI_WS_LISTEN_MODE_BUTTON

typedef enum {
    XIAOZHI_WS_EVT_WAIT_RESPONSE_TIMEOUT = 1,
    XIAOZHI_WS_EVT_SPEAKING_TIMEOUT,
    XIAOZHI_WS_EVT_AUTO_SILENCE_TIMEOUT,
    XIAOZHI_WS_EVT_AUTO_MAX_LISTEN_TIMEOUT,
    XIAOZHI_WS_EVT_TTS_RESUME_DELAY,
} xiaozhi_ws_session_event_t;

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
static esp_err_t wait_for_ready(uint32_t timeout_ms);
static void cleanup_websocket_client(void);
static void handle_websocket_write_failure(const char *stage, int sent, size_t expected_len);
static void stop_opus_audio_stream(void);
static esp_err_t ensure_websocket_ready(void);
static esp_err_t start_manual_listen_now(void);
static void start_pending_ptt_if_ready(void);
static esp_err_t send_listen_state(const char *state, const char *mode);
static esp_err_t start_audio_stream_with_flags(audio_opus_pcm_source_t pcm_source, int decoder_output_sample_rate, uint32_t flags);
static esp_err_t start_audio_stream_with_rate(audio_opus_pcm_source_t pcm_source, int decoder_output_sample_rate);
static esp_err_t restore_downlink_audio_stream(audio_opus_pcm_source_t pcm_source);
static esp_err_t start_sr_uplink_stream(void);
static esp_err_t ensure_downlink_audio_stream(void);
static esp_err_t restart_sr_after_downlink(void);
static bool ensure_session_task(void);
static bool xiaozhi_ws_post_session_event(xiaozhi_ws_session_event_t event);
static void xiaozhi_ws_session_task(void *arg);
static void handle_waiting_response_timeout_event(void);
static void handle_speaking_timeout_event(void);
static void handle_auto_silence_timeout_event(void);
static void handle_auto_max_listen_timeout_event(void);
static void handle_tts_resume_delay_event(void);
static void reset_session_flags(void);
static void waiting_response_timeout_cb(TimerHandle_t timer);
static void set_waiting_response(xiaozhi_ws_state_t last_state, uint32_t timeout_ms, const audio_opus_stream_stats_t *stats);
static void cancel_waiting_response_timer(void);
static void note_waiting_response_activity(const char *label);
static void speaking_timeout_cb(TimerHandle_t timer);
static bool ensure_speaking_timeout_timer(void);
static void cancel_speaking_timeout_timer(void);
static bool ensure_auto_endpoint_timers(void);
static void cancel_auto_endpoint_timers(void);
static void auto_silence_timeout_cb(TimerHandle_t timer);
static void auto_max_listen_timeout_cb(TimerHandle_t timer);
static bool ensure_tts_resume_timer(void);
static void cancel_tts_resume_timer(void);
static void tts_resume_timeout_cb(TimerHandle_t timer);
static void note_speaking_activity(const char *label);
static const char *listen_mode_name(xiaozhi_ws_listen_mode_t mode);
static void mark_listen_started(xiaozhi_ws_listen_mode_t mode);
static uint32_t current_listen_ms(void);
static uint32_t current_silence_ms(void);
static bool auto_listen_endpoint_ready(uint32_t listen_ms, uint32_t silence_ms, uint32_t tx_frames);
static void schedule_auto_silence_stop(void);

#if CONFIG_XIAOZHI_STAGE1_AUTO_SR_ENABLE
static bool s_auto_sr_downlink_active;
#endif

#define XIAOZHI_WS_READY_TIMEOUT_MS 10000
#define XIAOZHI_WS_READY_POLL_MS 100
#define XIAOZHI_WS_RESPONSE_TIMEOUT_MS 6500
#define XIAOZHI_WS_SHORT_RESPONSE_TIMEOUT_MS 1500
#define XIAOZHI_WS_SPEAKING_IDLE_TIMEOUT_MS 30000
#define XIAOZHI_WS_MIN_LISTEN_TX_FRAMES 18
#define XIAOZHI_WS_MIN_LISTEN_MS 1000
#define XIAOZHI_WS_AUTO_SILENCE_STOP_MS 1000
#define XIAOZHI_WS_AUTO_MAX_LISTEN_MS 12000
#define XIAOZHI_WS_TTS_RESUME_DELAY_MS 400
#define XIAOZHI_WS_SESSION_QUEUE_LENGTH 8
#define XIAOZHI_WS_SESSION_TASK_STACK_BYTES 4096
#define XIAOZHI_WS_SESSION_TASK_PRIORITY 5
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
    s_pending_ptt = false;
    s_button_down = false;
    s_manual_listen_start_tick = 0;
    s_listen_start_time_us = 0;
    s_vad_silence_start_time_us = 0;
    s_active_listen_mode = XIAOZHI_WS_LISTEN_MODE_AUTO;
    s_vad_muted_by_playback = false;
    s_waiting_response_timeout_ms = 0;
    memset(&s_waiting_response_stats, 0, sizeof(s_waiting_response_stats));
    cancel_waiting_response_timer();
    cancel_speaking_timeout_timer();
    cancel_auto_endpoint_timers();
    cancel_tts_resume_timer();
}

static const char *listen_mode_name(xiaozhi_ws_listen_mode_t mode)
{
    switch (mode) {
    case XIAOZHI_WS_LISTEN_MODE_AUTO:
        return "auto";
    case XIAOZHI_WS_LISTEN_MODE_BUTTON:
        return "manual";
    case XIAOZHI_WS_LISTEN_MODE_WAKE:
        return "wake";
    default:
        return "unknown";
    }
}

static void mark_listen_started(xiaozhi_ws_listen_mode_t mode)
{
    s_active_listen_mode = mode;
    s_listen_start_time_us = esp_timer_get_time();
    s_vad_silence_start_time_us = 0;
    s_manual_listen_start_tick = xTaskGetTickCount();
    ESP_LOGI(TAG,
             "listen timing start mode=%s start_us=%lld task=%s",
             listen_mode_name(mode),
             (long long)s_listen_start_time_us,
             pcTaskGetName(NULL));
}

static uint32_t elapsed_ms_since_us(int64_t start_us)
{
    if (start_us <= 0) {
        return 0;
    }
    int64_t delta_us = esp_timer_get_time() - start_us;
    if (delta_us <= 0) {
        return 0;
    }
    return (uint32_t)(delta_us / 1000);
}

static uint32_t current_listen_ms(void)
{
    return elapsed_ms_since_us(s_listen_start_time_us);
}

static uint32_t current_silence_ms(void)
{
    return elapsed_ms_since_us(s_vad_silence_start_time_us);
}

static bool auto_listen_endpoint_ready(uint32_t listen_ms, uint32_t silence_ms, uint32_t tx_frames)
{
    return listen_ms >= XIAOZHI_WS_MIN_LISTEN_MS &&
           silence_ms >= XIAOZHI_WS_AUTO_SILENCE_STOP_MS &&
           tx_frames >= XIAOZHI_WS_MIN_LISTEN_TX_FRAMES;
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

static bool ensure_waiting_response_timer(uint32_t timeout_ms)
{
    if (s_waiting_response_timer == NULL) {
        s_waiting_response_timer = xTimerCreate("xz_resp_to",
                                                pdMS_TO_TICKS(timeout_ms),
                                                pdFALSE,
                                                NULL,
                                                waiting_response_timeout_cb);
        if (s_waiting_response_timer == NULL) {
            ESP_LOGE(TAG, "create WAITING_RESPONSE timer failed");
            return false;
        }
    }

    return xTimerChangePeriod(s_waiting_response_timer, pdMS_TO_TICKS(timeout_ms), 0) == pdPASS;
}

static void cancel_waiting_response_timer(void)
{
    if (s_waiting_response_timer != NULL) {
        (void)xTimerStop(s_waiting_response_timer, 0);
    }
}

static void set_waiting_response(xiaozhi_ws_state_t last_state, uint32_t timeout_ms, const audio_opus_stream_stats_t *stats)
{
    s_waiting_response_last_state = last_state;
    s_waiting_response_timeout_ms = timeout_ms;
    if (stats != NULL) {
        s_waiting_response_stats = *stats;
    } else {
        memset(&s_waiting_response_stats, 0, sizeof(s_waiting_response_stats));
    }

    set_state(XIAOZHI_WS_STATE_WAITING_RESPONSE);
    if (!ensure_waiting_response_timer(timeout_ms)) {
        ESP_LOGW(TAG, "WAITING_RESPONSE timer unavailable, recover to READY");
        set_state(XIAOZHI_WS_STATE_READY);
    }
}

static void note_waiting_response_activity(const char *label)
{
    if (s_ws_state != XIAOZHI_WS_STATE_WAITING_RESPONSE) {
        return;
    }

    s_waiting_response_timeout_ms = XIAOZHI_WS_RESPONSE_TIMEOUT_MS;
    ESP_LOGI(TAG,
             "WAITING_RESPONSE activity=%s refresh_timeout=%u ms",
             label != NULL ? label : "<unknown>",
             (unsigned int)XIAOZHI_WS_RESPONSE_TIMEOUT_MS);
    (void)ensure_waiting_response_timer(XIAOZHI_WS_RESPONSE_TIMEOUT_MS);
}

static void waiting_response_timeout_cb(TimerHandle_t timer)
{
    (void)timer;
    ESP_LOGW(TAG, "WAITING_RESPONSE timer callback post event task=%s", pcTaskGetName(NULL));
    (void)xiaozhi_ws_post_session_event(XIAOZHI_WS_EVT_WAIT_RESPONSE_TIMEOUT);
}

static bool ensure_speaking_timeout_timer(void)
{
    if (s_speaking_timeout_timer == NULL) {
        s_speaking_timeout_timer = xTimerCreate("xz_tts_idle",
                                                pdMS_TO_TICKS(XIAOZHI_WS_SPEAKING_IDLE_TIMEOUT_MS),
                                                pdFALSE,
                                                NULL,
                                                speaking_timeout_cb);
        if (s_speaking_timeout_timer == NULL) {
            ESP_LOGE(TAG, "create SPEAKING watchdog timer failed");
            return false;
        }
    }

    return xTimerChangePeriod(s_speaking_timeout_timer, pdMS_TO_TICKS(XIAOZHI_WS_SPEAKING_IDLE_TIMEOUT_MS), 0) == pdPASS;
}

static void cancel_speaking_timeout_timer(void)
{
    if (s_speaking_timeout_timer != NULL) {
        (void)xTimerStop(s_speaking_timeout_timer, 0);
    }
}

static bool ensure_auto_endpoint_timers(void)
{
    if (s_auto_silence_timer == NULL) {
        s_auto_silence_timer = xTimerCreate("xz_auto_sil",
                                            pdMS_TO_TICKS(XIAOZHI_WS_AUTO_SILENCE_STOP_MS),
                                            pdFALSE,
                                            NULL,
                                            auto_silence_timeout_cb);
        if (s_auto_silence_timer == NULL) {
            ESP_LOGE(TAG, "create auto silence timer failed");
            return false;
        }
    }

    if (s_auto_max_listen_timer == NULL) {
        s_auto_max_listen_timer = xTimerCreate("xz_auto_max",
                                               pdMS_TO_TICKS(XIAOZHI_WS_AUTO_MAX_LISTEN_MS),
                                               pdFALSE,
                                               NULL,
                                               auto_max_listen_timeout_cb);
        if (s_auto_max_listen_timer == NULL) {
            ESP_LOGE(TAG, "create auto max listen timer failed");
            return false;
        }
    }

    return true;
}

static void cancel_auto_endpoint_timers(void)
{
    if (s_auto_silence_timer != NULL) {
        (void)xTimerStop(s_auto_silence_timer, 0);
    }
    if (s_auto_max_listen_timer != NULL) {
        (void)xTimerStop(s_auto_max_listen_timer, 0);
    }
}

static void auto_silence_timeout_cb(TimerHandle_t timer)
{
    (void)timer;
    ESP_LOGI(TAG, "AUTO_SILENCE timer callback post event task=%s", pcTaskGetName(NULL));
    (void)xiaozhi_ws_post_session_event(XIAOZHI_WS_EVT_AUTO_SILENCE_TIMEOUT);
}

static void auto_max_listen_timeout_cb(TimerHandle_t timer)
{
    (void)timer;
    ESP_LOGI(TAG, "AUTO_MAX_LISTEN timer callback post event task=%s", pcTaskGetName(NULL));
    (void)xiaozhi_ws_post_session_event(XIAOZHI_WS_EVT_AUTO_MAX_LISTEN_TIMEOUT);
}

static void schedule_auto_silence_stop(void)
{
    if (!ensure_auto_endpoint_timers()) {
        return;
    }

    uint32_t listen_ms = current_listen_ms();
    uint32_t silence_ms = current_silence_ms();
    uint32_t wait_ms = XIAOZHI_WS_AUTO_SILENCE_STOP_MS;
    if (silence_ms < XIAOZHI_WS_AUTO_SILENCE_STOP_MS) {
        wait_ms = XIAOZHI_WS_AUTO_SILENCE_STOP_MS - silence_ms;
    }
    if (listen_ms < XIAOZHI_WS_MIN_LISTEN_MS) {
        uint32_t min_wait_ms = XIAOZHI_WS_MIN_LISTEN_MS - listen_ms;
        if (min_wait_ms > wait_ms) {
            wait_ms = min_wait_ms;
        }
    }
    if (wait_ms == 0) {
        wait_ms = 1;
    }

    (void)xTimerChangePeriod(s_auto_silence_timer, pdMS_TO_TICKS(wait_ms), 0);
    ESP_LOGI(TAG,
             "auto silence stop scheduled wait_ms=%u listen_ms=%u silence_ms=%u task=%s",
             (unsigned int)wait_ms,
             (unsigned int)listen_ms,
             (unsigned int)silence_ms,
             pcTaskGetName(NULL));
}

static bool ensure_tts_resume_timer(void)
{
    if (s_tts_resume_timer == NULL) {
        s_tts_resume_timer = xTimerCreate("xz_tts_res",
                                          pdMS_TO_TICKS(XIAOZHI_WS_TTS_RESUME_DELAY_MS),
                                          pdFALSE,
                                          NULL,
                                          tts_resume_timeout_cb);
        if (s_tts_resume_timer == NULL) {
            ESP_LOGE(TAG, "create TTS resume timer failed");
            return false;
        }
    }
    return true;
}

static void cancel_tts_resume_timer(void)
{
    if (s_tts_resume_timer != NULL) {
        (void)xTimerStop(s_tts_resume_timer, 0);
    }
}

static void tts_resume_timeout_cb(TimerHandle_t timer)
{
    (void)timer;
    ESP_LOGI(TAG, "TTS_RESUME timer callback post event task=%s", pcTaskGetName(NULL));
    (void)xiaozhi_ws_post_session_event(XIAOZHI_WS_EVT_TTS_RESUME_DELAY);
}

static void note_speaking_activity(const char *label)
{
    if (s_ws_state != XIAOZHI_WS_STATE_SPEAKING) {
        return;
    }

    if (!ensure_speaking_timeout_timer()) {
        ESP_LOGW(TAG, "SPEAKING watchdog unavailable activity=%s", label != NULL ? label : "<unknown>");
    }
}

static void speaking_timeout_cb(TimerHandle_t timer)
{
    (void)timer;
    ESP_LOGW(TAG, "SPEAKING timer callback post event task=%s", pcTaskGetName(NULL));
    (void)xiaozhi_ws_post_session_event(XIAOZHI_WS_EVT_SPEAKING_TIMEOUT);
}

static bool ensure_session_task(void)
{
    if (s_session_event_queue == NULL) {
        s_session_event_queue = xQueueCreate(XIAOZHI_WS_SESSION_QUEUE_LENGTH, sizeof(xiaozhi_ws_session_event_t));
        if (s_session_event_queue == NULL) {
            ESP_LOGE(TAG, "create xiaozhi session event queue failed");
            return false;
        }
    }

    if (s_session_task_handle == NULL) {
        BaseType_t created = xTaskCreate(xiaozhi_ws_session_task,
                                         "xz_ws_sess",
                                         XIAOZHI_WS_SESSION_TASK_STACK_BYTES,
                                         NULL,
                                         XIAOZHI_WS_SESSION_TASK_PRIORITY,
                                         &s_session_task_handle);
        if (created != pdPASS) {
            ESP_LOGE(TAG, "create xiaozhi session task failed");
            return false;
        }
    }

    return true;
}

static bool xiaozhi_ws_post_session_event(xiaozhi_ws_session_event_t event)
{
    if (!ensure_session_task()) {
        return false;
    }

    BaseType_t sent = xQueueSend(s_session_event_queue, &event, 0);
    if (sent != pdTRUE) {
        ESP_LOGW(TAG, "drop session event=%d queue full task=%s", (int)event, pcTaskGetName(NULL));
        return false;
    }
    return true;
}

static void xiaozhi_ws_session_task(void *arg)
{
    (void)arg;
    xiaozhi_ws_session_event_t event = 0;
    while (true) {
        if (xQueueReceive(s_session_event_queue, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        switch (event) {
        case XIAOZHI_WS_EVT_WAIT_RESPONSE_TIMEOUT:
            handle_waiting_response_timeout_event();
            break;
        case XIAOZHI_WS_EVT_SPEAKING_TIMEOUT:
            handle_speaking_timeout_event();
            break;
        case XIAOZHI_WS_EVT_AUTO_SILENCE_TIMEOUT:
            handle_auto_silence_timeout_event();
            break;
        case XIAOZHI_WS_EVT_AUTO_MAX_LISTEN_TIMEOUT:
            handle_auto_max_listen_timeout_event();
            break;
        case XIAOZHI_WS_EVT_TTS_RESUME_DELAY:
            handle_tts_resume_delay_event();
            break;
        default:
            ESP_LOGW(TAG, "unknown session event=%d task=%s", (int)event, pcTaskGetName(NULL));
            break;
        }
    }
}

static void handle_waiting_response_timeout_event(void)
{
    ESP_LOGW(TAG,
             "WAITING_RESPONSE timeout event task=%s state=%s timeout=%u tx_frames=%u tx_bytes=%u last_state=%s internal_free=%u internal_largest=%u",
             pcTaskGetName(NULL),
             state_name(s_ws_state),
             (unsigned int)s_waiting_response_timeout_ms,
             (unsigned int)s_waiting_response_stats.tx_frames,
             (unsigned int)s_waiting_response_stats.tx_bytes,
             state_name(s_waiting_response_last_state),
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    if (s_ws_state != XIAOZHI_WS_STATE_WAITING_RESPONSE) {
        ESP_LOGI(TAG, "WAITING_RESPONSE timeout event ignored state=%s task=%s", state_name(s_ws_state), pcTaskGetName(NULL));
        return;
    }

    s_waiting_tts_stop = false;
    set_state(XIAOZHI_WS_STATE_READY);
    ESP_LOGI(TAG,
             "WAITING_RESPONSE timeout event handled state=%s internal_free=%u internal_largest=%u task=%s",
             state_name(s_ws_state),
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             pcTaskGetName(NULL));
}

static void handle_speaking_timeout_event(void)
{
    ESP_LOGW(TAG,
             "SPEAKING timeout event task=%s state=%s timeout=%u",
             pcTaskGetName(NULL),
             state_name(s_ws_state),
             (unsigned int)XIAOZHI_WS_SPEAKING_IDLE_TIMEOUT_MS);
    if (s_ws_state != XIAOZHI_WS_STATE_SPEAKING) {
        return;
    }

    s_waiting_tts_stop = false;
    esp_err_t err = restart_sr_after_downlink();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "speaking recovery failed: %s", esp_err_to_name(err));
    }
    set_state(err == ESP_OK ? XIAOZHI_WS_STATE_READY : XIAOZHI_WS_STATE_DISCONNECTED);
}

static void handle_auto_silence_timeout_event(void)
{
    if (s_ws_state != XIAOZHI_WS_STATE_LISTENING || s_active_listen_mode != XIAOZHI_WS_LISTEN_MODE_AUTO) {
        ESP_LOGI(TAG, "auto silence event ignored state=%s mode=%s task=%s", state_name(s_ws_state), listen_mode_name(s_active_listen_mode), pcTaskGetName(NULL));
        return;
    }

    audio_opus_stream_stats_t stats = {0};
    audio_opus_stream_get_stats(&stats);
    uint32_t listen_ms = current_listen_ms();
    uint32_t silence_ms = current_silence_ms();
    ESP_LOGI(TAG,
             "auto silence event mode=auto listen_ms=%u silence_ms=%u tx_frames=%u tx_bytes=%u task=%s",
             (unsigned int)listen_ms,
             (unsigned int)silence_ms,
             (unsigned int)stats.tx_frames,
             (unsigned int)stats.tx_bytes,
             pcTaskGetName(NULL));

    if (!auto_listen_endpoint_ready(listen_ms, silence_ms, stats.tx_frames)) {
        schedule_auto_silence_stop();
        return;
    }

    (void)xiaozhi_ws_stop_listen();
}

static void handle_auto_max_listen_timeout_event(void)
{
    if (s_ws_state != XIAOZHI_WS_STATE_LISTENING || s_active_listen_mode != XIAOZHI_WS_LISTEN_MODE_AUTO) {
        ESP_LOGI(TAG, "auto max listen event ignored state=%s mode=%s task=%s", state_name(s_ws_state), listen_mode_name(s_active_listen_mode), pcTaskGetName(NULL));
        return;
    }

    audio_opus_stream_stats_t stats = {0};
    audio_opus_stream_get_stats(&stats);
    ESP_LOGW(TAG,
             "auto max listen event mode=auto listen_ms=%u silence_ms=%u tx_frames=%u tx_bytes=%u task=%s",
             (unsigned int)current_listen_ms(),
             (unsigned int)current_silence_ms(),
             (unsigned int)stats.tx_frames,
             (unsigned int)stats.tx_bytes,
             pcTaskGetName(NULL));
    (void)xiaozhi_ws_stop_listen();
}

static void handle_tts_resume_delay_event(void)
{
    ESP_LOGI(TAG, "TTS resume delay event state=%s task=%s", state_name(s_ws_state), pcTaskGetName(NULL));
    s_vad_muted_by_playback = false;
    esp_err_t err = restart_sr_after_downlink();
    s_waiting_tts_stop = false;
    set_state(err == ESP_OK ? XIAOZHI_WS_STATE_READY : XIAOZHI_WS_STATE_DISCONNECTED);
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
        .flags = 0,
    };
    return audio_opus_stream_start(&config);
}

static esp_err_t start_audio_stream_with_flags(audio_opus_pcm_source_t pcm_source, int decoder_output_sample_rate, uint32_t flags)
{
    const audio_opus_stream_config_t config = {
        .send_cb = send_opus_frame,
        .user_ctx = NULL,
        .output_volume = -1,
        .pcm_source = pcm_source,
        .decoder_output_sample_rate = decoder_output_sample_rate,
        .flags = flags,
    };
    return audio_opus_stream_start(&config);
}

static esp_err_t start_audio_stream_with_rate(audio_opus_pcm_source_t pcm_source, int decoder_output_sample_rate)
{
    return start_audio_stream_with_flags(pcm_source, decoder_output_sample_rate, 0);
}

static esp_err_t start_sr_uplink_stream(void)
{
    (void)audio_opus_stream_set_uplink_enabled(false);
    audio_opus_stream_flush();
    stop_opus_audio_stream();

#if CONFIG_XIAOZHI_STAGE1_AUTO_SR_ENABLE
    s_auto_sr_downlink_active = false;
#endif

    ESP_LOGI(TAG, "start SR external PCM uplink stream");
    return start_audio_stream_with_flags(AUDIO_OPUS_PCM_SOURCE_EXTERNAL_FEED,
                                         AUDIO_OPUS_SAMPLE_RATE,
                                         AUDIO_OPUS_STREAM_FLAG_SKIP_AUDIO_PATH_OPEN |
                                             AUDIO_OPUS_STREAM_FLAG_UPLINK_ONLY);
}

static esp_err_t ensure_downlink_audio_stream(void)
{
#if CONFIG_XIAOZHI_STAGE1_AUTO_SR_ENABLE
    if (s_auto_sr_downlink_active) {
        return ESP_OK;
    }

    esp_err_t pause_err = xiaozhi_sr_pause();
    ESP_RETURN_ON_ERROR(pause_err, TAG, "pause SR before downlink failed");
    (void)audio_opus_stream_set_uplink_enabled(false);
    audio_opus_stream_flush();
    stop_opus_audio_stream();

    ESP_LOGI(TAG,
             "prepare downlink audio path current_sample_rate=%d target_sample_rate=%d",
             bsp_audio_get_current_sample_rate(),
             resolve_decoder_output_sample_rate());
    esp_err_t err = start_audio_stream(AUDIO_OPUS_PCM_SOURCE_EXTERNAL_FEED);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start downlink opus stream failed: %s", esp_err_to_name(err));
        (void)xiaozhi_sr_resume();
        return err;
    }
    (void)audio_opus_stream_set_uplink_enabled(false);
    audio_opus_stream_flush();
    s_auto_sr_downlink_active = true;
    return ESP_OK;
#else
    return start_audio_stream(AUDIO_OPUS_PCM_SOURCE_EXTERNAL_FEED);
#endif
}

static esp_err_t restart_sr_after_downlink(void)
{
#if CONFIG_XIAOZHI_STAGE1_AUTO_SR_ENABLE
    (void)audio_opus_stream_set_uplink_enabled(false);
    (void)audio_opus_stream_wait_downlink_idle(XIAOZHI_WS_DOWNLINK_DRAIN_TIMEOUT_MS);
    (void)audio_opus_stream_close_decoder();
    stop_opus_audio_stream();
    s_auto_sr_downlink_active = false;
    esp_err_t resume_err = xiaozhi_sr_resume();
    ESP_RETURN_ON_ERROR(resume_err, TAG, "resume SR after downlink failed");
    return ESP_OK;
#else
    (void)audio_opus_stream_close_decoder();
    return ESP_OK;
#endif
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
#if CONFIG_XIAOZHI_STAGE1_AUTO_SR_ENABLE
    s_auto_sr_downlink_active = false;
    (void)xiaozhi_sr_resume();
#endif
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

#if CONFIG_XIAOZHI_STAGE1_AUTO_SR_ENABLE
    ESP_LOGI(TAG, "automatic SR mode keeps codec owned by WakeNet/VAD until downlink playback");
#else
    esp_err_t err = start_audio_stream(AUDIO_OPUS_PCM_SOURCE_EXTERNAL_FEED);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start opus voice stream failed: %s", esp_err_to_name(err));
        set_state(XIAOZHI_WS_STATE_DISCONNECTED);
        return;
    }
    (void)audio_opus_stream_set_uplink_enabled(false);
    audio_opus_stream_flush();
#endif

    set_state(XIAOZHI_WS_STATE_READY);
    log_heap_stats("WS READY");
    audio_opus_stream_log_watermarks("WS READY");
    start_pending_ptt_if_ready();
}

static void handle_tts(const xiaozhi_protocol_msg_t *msg)
{
    if (strcmp(msg->state, "start") == 0) {
        ESP_LOGI(TAG, "tts start");
        cancel_waiting_response_timer();
        s_waiting_tts_stop = true;
        esp_err_t err = ensure_downlink_audio_stream();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "prepare downlink on tts start failed: %s", esp_err_to_name(err));
            set_state(XIAOZHI_WS_STATE_DISCONNECTED);
            return;
        }
        (void)audio_opus_stream_set_uplink_enabled(false);
        set_state(XIAOZHI_WS_STATE_SPEAKING);
        note_speaking_activity("tts start");
        log_heap_stats("TTS start");
        audio_opus_stream_log_watermarks("TTS start");
        return;
    }

    if (strcmp(msg->state, "stop") == 0) {
        ESP_LOGI(TAG, "tts stop");
        cancel_waiting_response_timer();
        cancel_speaking_timeout_timer();
        (void)audio_opus_stream_set_uplink_enabled(false);
        esp_err_t err = restart_sr_after_downlink();
        s_waiting_tts_stop = false;
        set_state(err == ESP_OK ? XIAOZHI_WS_STATE_READY : XIAOZHI_WS_STATE_DISCONNECTED);
        ESP_LOGI(TAG, "tts stop -> READY");
        log_heap_stats("TTS stop");
        audio_opus_stream_log_watermarks("TTS stop");
        return;
    }

    ESP_LOGI(TAG, "tts state=%s", msg->state[0] != '\0' ? msg->state : "<empty>");
    note_waiting_response_activity("tts");
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
        note_waiting_response_activity("stt");
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
        note_waiting_response_activity("llm");
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
        cancel_waiting_response_timer();
        s_waiting_tts_stop = true;
        (void)audio_opus_stream_set_uplink_enabled(false);
        set_state(XIAOZHI_WS_STATE_SPEAKING);
    }
#if CONFIG_XIAOZHI_STAGE1_AUTO_SR_ENABLE
    (void)xiaozhi_sr_pause();
#endif
    esp_err_t prepare_err = ensure_downlink_audio_stream();
    if (prepare_err != ESP_OK) {
        ESP_LOGW(TAG, "prepare downlink for binary opus failed: %s", esp_err_to_name(prepare_err));
        set_state(XIAOZHI_WS_STATE_DISCONNECTED);
        return;
    }
    note_speaking_activity("binary opus");

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

esp_err_t xiaozhi_ws_request_ready(void)
{
    if (is_ready_or_busy_state(s_ws_state) ||
        s_ws_state == XIAOZHI_WS_STATE_CONNECTING ||
        s_ws_state == XIAOZHI_WS_STATE_WS_CONNECTED ||
        s_ws_state == XIAOZHI_WS_STATE_HELLO_SENT) {
        return ESP_OK;
    }

    if (s_ws_state == XIAOZHI_WS_STATE_DISCONNECTED) {
        return xiaozhi_ws_start();
    }

    ESP_LOGW(TAG, "websocket READY request ignored state=%s", state_name(s_ws_state));
    return ESP_ERR_INVALID_STATE;
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

    s_button_down = true;

    if (s_ws_state == XIAOZHI_WS_STATE_LISTENING) {
        ESP_LOGI(TAG, "manual listen start ignored because already listening");
        return ESP_OK;
    }

    if (s_ws_state == XIAOZHI_WS_STATE_SPEAKING || s_waiting_tts_stop) {
        s_pending_ptt = false;
        ESP_LOGW(TAG,
                 "manual listen start ignored while TTS is active state=%s waiting_tts_stop=%d",
                 state_name(s_ws_state),
                 s_waiting_tts_stop);
        return ESP_ERR_INVALID_STATE;
    }

    if (s_ws_state == XIAOZHI_WS_STATE_WAITING_RESPONSE) {
        s_pending_ptt = false;
        ESP_LOGW(TAG, "manual listen start ignored while waiting response");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_ws_state == XIAOZHI_WS_STATE_READY || s_ws_state == XIAOZHI_WS_STATE_WAKE_DETECTED) {
        s_pending_ptt = false;
        return start_manual_listen_now();
    }

    s_pending_ptt = true;
    esp_err_t err = xiaozhi_ws_request_ready();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "manual listen pending but websocket READY request failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "manual listen pending until READY state=%s button_down=%d", state_name(s_ws_state), s_button_down);
    return ESP_OK;
}

static esp_err_t start_manual_listen_now(void)
{
    if (s_session_id[0] == '\0') {
        ESP_LOGW(TAG, "manual listen start ignored because session_id is missing");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_ws_state != XIAOZHI_WS_STATE_READY && s_ws_state != XIAOZHI_WS_STATE_WAKE_DETECTED) {
        ESP_LOGW(TAG, "manual listen start invalid state=%s", state_name(s_ws_state));
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ESP_OK;
#if CONFIG_XIAOZHI_STAGE1_AUTO_SR_ENABLE
    err = start_sr_uplink_stream();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start manual SR external PCM stream failed: %s", esp_err_to_name(err));
        return err;
    }
#else
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
#endif

    err = send_listen_state("start", "manual");
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "manual listen start send failed: %s", esp_err_to_name(err));
        (void)audio_opus_stream_set_uplink_enabled(false);
        audio_opus_stream_flush();
#if CONFIG_XIAOZHI_STAGE1_AUTO_SR_ENABLE
        stop_opus_audio_stream();
#else
        (void)restore_downlink_audio_stream(AUDIO_OPUS_PCM_SOURCE_EXTERNAL_FEED);
#endif
        return err;
    }

    s_next_opus_send_tick = 0;
    s_manual_listen_start_tick = xTaskGetTickCount();
    cancel_waiting_response_timer();
    set_state(XIAOZHI_WS_STATE_LISTENING);

    err = audio_opus_stream_set_uplink_enabled(true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "manual listen uplink enable failed: %s", esp_err_to_name(err));
        (void)send_listen_state("stop", "manual");
#if CONFIG_XIAOZHI_STAGE1_AUTO_SR_ENABLE
        stop_opus_audio_stream();
        (void)xiaozhi_sr_resume();
        audio_opus_stream_stats_t stats = {0};
        set_waiting_response(XIAOZHI_WS_STATE_LISTENING, XIAOZHI_WS_SHORT_RESPONSE_TIMEOUT_MS, &stats);
#else
        esp_err_t restore_err = restore_downlink_audio_stream(AUDIO_OPUS_PCM_SOURCE_EXTERNAL_FEED);
        if (restore_err == ESP_OK) {
            audio_opus_stream_stats_t stats = {0};
            set_waiting_response(XIAOZHI_WS_STATE_LISTENING, XIAOZHI_WS_SHORT_RESPONSE_TIMEOUT_MS, &stats);
        } else {
            set_state(XIAOZHI_WS_STATE_DISCONNECTED);
        }
#endif
        return err;
    }

    log_heap_stats("listen start manual");
    audio_opus_stream_log_watermarks("listen start manual");
    return ESP_OK;
}

static void start_pending_ptt_if_ready(void)
{
    if (!s_pending_ptt) {
        return;
    }

    if (!s_button_down) {
        ESP_LOGI(TAG, "manual listen pending canceled before READY");
        s_pending_ptt = false;
        return;
    }

    if (s_ws_state != XIAOZHI_WS_STATE_READY) {
        ESP_LOGI(TAG, "manual listen pending waits for READY state=%s", state_name(s_ws_state));
        return;
    }

    s_pending_ptt = false;
    ESP_LOGI(TAG, "manual listen pending accepted after READY button_down=%d", s_button_down);
    esp_err_t err = start_manual_listen_now();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "manual listen pending start failed: %s", esp_err_to_name(err));
    }
}

static esp_err_t ensure_websocket_ready(void)
{
    ESP_RETURN_ON_ERROR(xiaozhi_ws_request_ready(), TAG, "request websocket READY failed");

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

#if CONFIG_XIAOZHI_STAGE1_AUTO_SR_ENABLE
        err = start_sr_uplink_stream();
        if (err != ESP_OK) {
            return err;
        }

        err = send_listen_state("start", "auto");
#else
        err = send_listen_state("start", "manual");
#endif
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

    if (s_ws_state != XIAOZHI_WS_STATE_READY && s_ws_state != XIAOZHI_WS_STATE_WAKE_DETECTED) {
        esp_err_t ready_err = xiaozhi_ws_request_ready();
        ESP_LOGW(TAG,
                 "detect text ignored until READY state=%s request_ready=%s",
                 state_name(s_ws_state),
                 esp_err_to_name(ready_err));
        return ready_err == ESP_OK ? ESP_ERR_INVALID_STATE : ready_err;
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

    esp_err_t err = send_text_json(json, "listen detect");
    if (err == ESP_OK) {
        set_waiting_response(s_ws_state, XIAOZHI_WS_RESPONSE_TIMEOUT_MS, NULL);
        log_heap_stats("listen detect sent");
    } else {
        set_state(XIAOZHI_WS_STATE_DISCONNECTED);
    }
    return err;
}

esp_err_t xiaozhi_ws_stop_listen(void)
{
    s_button_down = false;
    if (s_ws_state != XIAOZHI_WS_STATE_LISTENING) {
        s_manual_listen_start_tick = 0;
        if (s_pending_ptt) {
            ESP_LOGI(TAG, "manual listen pending canceled by release state=%s", state_name(s_ws_state));
            s_pending_ptt = false;
        }
        if (s_ws_state == XIAOZHI_WS_STATE_WAKE_DETECTED) {
            set_state(XIAOZHI_WS_STATE_READY);
        }
        return ESP_OK;
    }

    s_pending_ptt = false;
    audio_opus_stream_stats_t stats = {0};
    audio_opus_stream_get_stats(&stats);
    uint32_t listen_ms = 0;
    if (s_manual_listen_start_tick != 0) {
        listen_ms = (uint32_t)(pdTICKS_TO_MS(xTaskGetTickCount() - s_manual_listen_start_tick));
    }
    s_manual_listen_start_tick = 0;
    uint32_t response_timeout_ms = XIAOZHI_WS_RESPONSE_TIMEOUT_MS;
    if (stats.tx_frames < XIAOZHI_WS_MIN_LISTEN_TX_FRAMES || listen_ms < XIAOZHI_WS_MIN_LISTEN_MS) {
        response_timeout_ms = XIAOZHI_WS_SHORT_RESPONSE_TIMEOUT_MS;
        ESP_LOGW(TAG,
                 "manual listen too short tx_frames=%u listen_ms=%u, response timeout=%u ms",
                 (unsigned int)stats.tx_frames,
                 (unsigned int)listen_ms,
                 (unsigned int)response_timeout_ms);
    }

    (void)audio_opus_stream_set_uplink_enabled(false);
    audio_opus_stream_flush();
    char *json = NULL;
    ESP_RETURN_ON_ERROR(xiaozhi_protocol_build_listen_stop_json(s_session_id, &json), TAG, "build listen stop failed");
    esp_err_t err = send_text_json(json, "listen stop");
#if CONFIG_XIAOZHI_STAGE1_AUTO_SR_ENABLE
    stop_opus_audio_stream();
    esp_err_t restore_err = xiaozhi_sr_resume();
#else
    const audio_opus_pcm_source_t restore_source = AUDIO_OPUS_PCM_SOURCE_EXTERNAL_FEED;
    esp_err_t restore_err = restore_downlink_audio_stream(restore_source);
#endif
    if (err == ESP_OK && restore_err == ESP_OK) {
        set_waiting_response(XIAOZHI_WS_STATE_LISTENING, response_timeout_ms, &stats);
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
