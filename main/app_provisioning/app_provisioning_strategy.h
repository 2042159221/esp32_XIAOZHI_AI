#pragma once

#include "esp_err.h"
#include "wifi_provisioning/manager.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct app_provisioning_strategy app_provisioning_strategy_t;

struct app_provisioning_strategy {
    const char *name;
    const char *transport;
    const char *qr_version;
    const char *capabilities;
    wifi_prov_mgr_config_t (*create_manager_config)(wifi_prov_cb_func_t event_cb, void *user_data);
    esp_err_t (*start)(const app_provisioning_strategy_t *strategy, const char *service_name, const char *service_key);
};

const app_provisioning_strategy_t *app_provisioning_strategy_factory_create(void);

#ifdef __cplusplus
}
#endif