#pragma once

#include "esp_err.h"
#include "wifi_provisioning/manager.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct esp_prov_strategy esp_prov_strategy_t;

struct esp_prov_strategy {
    const char *name;
    const char *transport;
    const char *qr_version;
    const char *capabilities;
    wifi_prov_mgr_config_t (*create_manager_config)(wifi_prov_cb_func_t event_cb, void *user_data);
    esp_err_t (*start)(const esp_prov_strategy_t *strategy, const char *service_name, const char *service_key);
};

const esp_prov_strategy_t *esp_prov_strategy_factory_create_primary(void);
const esp_prov_strategy_t *esp_prov_strategy_factory_create_fallback(const esp_prov_strategy_t *current);

#ifdef __cplusplus
}
#endif
