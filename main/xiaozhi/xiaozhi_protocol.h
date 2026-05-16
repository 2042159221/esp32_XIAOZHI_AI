#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XIAOZHI_PROTOCOL_VERSION 1
#define XIAOZHI_PROTOCOL_TRANSPORT "websocket"
#define XIAOZHI_PROTOCOL_AUDIO_FORMAT "opus"
#define XIAOZHI_PROTOCOL_AUDIO_SAMPLE_RATE 16000
#define XIAOZHI_PROTOCOL_AUDIO_CHANNELS 1
#define XIAOZHI_PROTOCOL_AUDIO_FRAME_DURATION_MS 60
#define XIAOZHI_PROTOCOL_SESSION_ID_MAX_LEN 64
#define XIAOZHI_PROTOCOL_TEXT_MAX_LEN 192
#define XIAOZHI_PROTOCOL_STATE_MAX_LEN 32
#define XIAOZHI_PROTOCOL_TRANSPORT_MAX_LEN 16
#define XIAOZHI_PROTOCOL_AUDIO_FORMAT_MAX_LEN 16

typedef enum {
    XIAOZHI_PROTOCOL_MSG_HELLO = 0,
    XIAOZHI_PROTOCOL_MSG_STT,
    XIAOZHI_PROTOCOL_MSG_TTS,
    XIAOZHI_PROTOCOL_MSG_LLM,
    XIAOZHI_PROTOCOL_MSG_MCP,
    XIAOZHI_PROTOCOL_MSG_SYSTEM,
    XIAOZHI_PROTOCOL_MSG_ALERT,
    XIAOZHI_PROTOCOL_MSG_UNKNOWN,
} xiaozhi_protocol_msg_type_t;

typedef struct {
    char format[XIAOZHI_PROTOCOL_AUDIO_FORMAT_MAX_LEN];
    int sample_rate;
    int channels;
    int frame_duration_ms;
} xiaozhi_protocol_audio_params_t;

typedef struct {
    xiaozhi_protocol_msg_type_t type;
    char session_id[XIAOZHI_PROTOCOL_SESSION_ID_MAX_LEN];
    char transport[XIAOZHI_PROTOCOL_TRANSPORT_MAX_LEN];
    char state[XIAOZHI_PROTOCOL_STATE_MAX_LEN];
    char text[XIAOZHI_PROTOCOL_TEXT_MAX_LEN];
    xiaozhi_protocol_audio_params_t audio;
} xiaozhi_protocol_msg_t;

esp_err_t xiaozhi_protocol_build_hello_json(char **out_json);
esp_err_t xiaozhi_protocol_build_listen_start_json(const char *session_id, const char *mode, char **out_json);
esp_err_t xiaozhi_protocol_build_listen_stop_json(const char *session_id, char **out_json);
esp_err_t xiaozhi_protocol_build_listen_detect_json(const char *session_id, const char *text, char **out_json);
esp_err_t xiaozhi_protocol_build_abort_json(const char *session_id, const char *reason, char **out_json);
esp_err_t xiaozhi_protocol_parse_server_message(const char *json, size_t len, xiaozhi_protocol_msg_t *out_msg);

#define build_hello_json xiaozhi_protocol_build_hello_json
#define build_listen_start_json xiaozhi_protocol_build_listen_start_json
#define build_listen_stop_json xiaozhi_protocol_build_listen_stop_json
#define build_listen_detect_json xiaozhi_protocol_build_listen_detect_json
#define build_abort_json xiaozhi_protocol_build_abort_json
#define parse_server_message xiaozhi_protocol_parse_server_message

#ifdef __cplusplus
}
#endif
