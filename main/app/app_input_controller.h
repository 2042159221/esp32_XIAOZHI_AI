#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef esp_err_t (*app_input_reset_provisioning_cb_t)(void *user_ctx);

typedef struct {
    app_input_reset_provisioning_cb_t reset_provisioning_cb;
    void *user_ctx;
} app_input_controller_config_t;

esp_err_t app_input_controller_init(const app_input_controller_config_t *config);

#ifdef __cplusplus
}
#endif
