#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_PROVISIONING_STATE_UNPROVISIONED = 0,
    APP_PROVISIONING_STATE_PROVISIONING,
    APP_PROVISIONING_STATE_CRED_RECEIVED,
    APP_PROVISIONING_STATE_CONNECTING,
    APP_PROVISIONING_STATE_CONNECTED,
    APP_PROVISIONING_STATE_FAILED,
    APP_PROVISIONING_STATE_BUSINESS_STARTED,
} app_provisioning_state_t;

typedef void (*app_provisioning_business_start_cb_t)(void *user_ctx);
typedef void (*app_provisioning_state_cb_t)(app_provisioning_state_t state, void *user_ctx);

typedef struct {
    app_provisioning_business_start_cb_t business_start_cb;
    app_provisioning_state_cb_t state_cb;
    void *user_ctx;
} app_provisioning_manager_config_t;

esp_err_t app_provisioning_manager_start(const app_provisioning_manager_config_t *config);
esp_err_t app_provisioning_manager_reset_and_restart(void);
app_provisioning_state_t app_provisioning_manager_get_state(void);

#ifdef __cplusplus
}
#endif