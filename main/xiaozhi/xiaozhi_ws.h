#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    XIAOZHI_WS_STATE_IDLE = 0,
    XIAOZHI_WS_STATE_CONNECTING,
    XIAOZHI_WS_STATE_CONNECTED,
    XIAOZHI_WS_STATE_HELLO_SENT,
    XIAOZHI_WS_STATE_READY,
    XIAOZHI_WS_STATE_LISTEN_STARTING,
    XIAOZHI_WS_STATE_LISTEN_STARTED,
    XIAOZHI_WS_STATE_STREAMING,
    XIAOZHI_WS_STATE_LISTENING,
    XIAOZHI_WS_STATE_SPEAKING,
    XIAOZHI_WS_STATE_STOPPING,
    XIAOZHI_WS_STATE_RECONNECTING,
    XIAOZHI_WS_STATE_DISCONNECTED,
    XIAOZHI_WS_STATE_ERROR
} xiaozhi_ws_state_t;

typedef enum {
    XIAOZHI_WS_LISTEN_MODE_AUTO = 0,
    XIAOZHI_WS_LISTEN_MODE_BUTTON,
    XIAOZHI_WS_LISTEN_MODE_WAKE,
} xiaozhi_ws_listen_mode_t;

esp_err_t xiaozhi_ws_start(void);
esp_err_t xiaozhi_ws_stop(void);
xiaozhi_ws_state_t xiaozhi_ws_get_state(void);
esp_err_t xiaozhi_ws_trigger_listen(xiaozhi_ws_listen_mode_t mode);
esp_err_t xiaozhi_ws_stop_listen(void);
esp_err_t xiaozhi_ws_abort_listening(const char *reason);
esp_err_t xiaozhi_ws_notify_server_hello(const char *json, size_t len);
esp_err_t xiaozhi_ws_notify_binary_opus(const uint8_t *data, size_t len);
esp_err_t xiaozhi_ws_notify_text(const char *text);
bool xiaozhi_ws_is_ready(void);

#ifdef __cplusplus
}
#endif
