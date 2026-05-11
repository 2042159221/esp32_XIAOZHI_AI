#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char *websocket_url;
    char *websocket_token;
    char *activation_code;
    char *activation_message;
    char *activation_challenge;
    bool is_activated;
} xiaozhi_handle_t;

extern xiaozhi_handle_t g_xiaozhi_handle;

esp_err_t xiaozhi_handle_init(void);
void xiaozhi_handle_deinit(void);
void xiaozhi_handle_clear_runtime(void);

esp_err_t xiaozhi_handle_set_websocket(const char *url, const char *token);
esp_err_t xiaozhi_handle_set_activation(const char *code, const char *message, const char *challenge);
void xiaozhi_handle_set_activated(bool activated);

const char *xiaozhi_handle_get_websocket_url(void);
const char *xiaozhi_handle_get_websocket_token(void);
const char *xiaozhi_handle_get_activation_code(void);
const char *xiaozhi_handle_get_activation_message(void);
const char *xiaozhi_handle_get_activation_challenge(void);
bool xiaozhi_handle_is_activated(void);

#ifdef __cplusplus
}
#endif
