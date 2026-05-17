#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef esp_err_t (*app_input_reset_provisioning_cb_t)(void *user_ctx);
typedef esp_err_t (*app_input_voice_trigger_cb_t)(void *user_ctx);

typedef enum {
    APP_INPUT_VOICE_EVT_TEXT_TEST = 0,
    APP_INPUT_VOICE_EVT_LISTEN_START,
    APP_INPUT_VOICE_EVT_LISTEN_STOP,
} app_input_voice_evt_t;

typedef esp_err_t (*app_input_voice_event_cb_t)(app_input_voice_evt_t evt, void *user_ctx);

typedef struct {
    app_input_reset_provisioning_cb_t reset_provisioning_cb;
    app_input_voice_trigger_cb_t voice_trigger_cb;
    app_input_voice_event_cb_t voice_event_cb;
    void *user_ctx;
} app_input_controller_config_t;

esp_err_t app_input_controller_init(const app_input_controller_config_t *config);

#ifdef __cplusplus
}
#endif
