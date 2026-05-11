#include "xiaozhi_ws.h"

#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "xiaozhi_handle.h"

static const char *TAG = "xiaozhi_ws";

static xiaozhi_ws_state_t s_ws_state = XIAOZHI_WS_STATE_IDLE;

static void log_token_summary(const char *token)
{
    if (token == NULL) {
        ESP_LOGI(TAG, "websocket token: <null>");
        return;
    }

    size_t len = strlen(token);
    ESP_LOGI(TAG, "websocket token present, len=%u", (unsigned int)len);
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

    s_ws_state = XIAOZHI_WS_STATE_CONNECTING;
    ESP_LOGI(TAG, "websocket placeholder ready, url=%s", url);
    log_token_summary(token);
    ESP_LOGI(TAG, "audio protocol is intentionally not implemented in stage1");
    s_ws_state = XIAOZHI_WS_STATE_CONNECTED;
    return ESP_OK;
}

esp_err_t xiaozhi_ws_stop(void)
{
    if (s_ws_state == XIAOZHI_WS_STATE_IDLE || s_ws_state == XIAOZHI_WS_STATE_DISCONNECTED) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "websocket placeholder stopped");
    s_ws_state = XIAOZHI_WS_STATE_DISCONNECTED;
    return ESP_OK;
}

xiaozhi_ws_state_t xiaozhi_ws_get_state(void)
{
    return s_ws_state;
}
