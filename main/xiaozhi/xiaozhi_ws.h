#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    XIAOZHI_WS_STATE_IDLE = 0,
    XIAOZHI_WS_STATE_CONNECTING,
    XIAOZHI_WS_STATE_CONNECTED,
    XIAOZHI_WS_STATE_DISCONNECTED,
    XIAOZHI_WS_STATE_ERROR
} xiaozhi_ws_state_t;

esp_err_t xiaozhi_ws_start(void);
esp_err_t xiaozhi_ws_stop(void);
xiaozhi_ws_state_t xiaozhi_ws_get_state(void);

#ifdef __cplusplus
}
#endif
