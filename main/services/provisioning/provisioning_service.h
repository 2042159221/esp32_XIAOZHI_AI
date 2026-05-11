#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PROVISIONING_SERVICE_STATE_UNPROVISIONED = 0,
    PROVISIONING_SERVICE_STATE_PROVISIONING,
    PROVISIONING_SERVICE_STATE_CRED_RECEIVED,
    PROVISIONING_SERVICE_STATE_CONNECTING,
    PROVISIONING_SERVICE_STATE_CONNECTED,
    PROVISIONING_SERVICE_STATE_FAILED,
    PROVISIONING_SERVICE_STATE_BUSINESS_STARTED,
} provisioning_service_state_t;

typedef void (*provisioning_service_business_start_cb_t)(void *user_ctx);
typedef void (*provisioning_service_state_cb_t)(provisioning_service_state_t state, void *user_ctx);
typedef void (*provisioning_service_qrcode_cb_t)(const char *payload, void *user_ctx);

typedef struct {
    provisioning_service_business_start_cb_t business_start_cb;
    provisioning_service_state_cb_t state_cb;
    provisioning_service_qrcode_cb_t qrcode_cb;
    void *user_ctx;
} provisioning_service_config_t;

esp_err_t provisioning_service_start(const provisioning_service_config_t *config);
esp_err_t provisioning_service_reset_and_restart(void);
provisioning_service_state_t provisioning_service_get_state(void);

#ifdef __cplusplus
}
#endif
