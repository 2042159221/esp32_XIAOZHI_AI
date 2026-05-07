#pragma once

#include <stdbool.h>

#include "app_provisioning_strategy.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t app_provisioning_adapter_init(const app_provisioning_strategy_t *strategy, wifi_prov_cb_func_t event_cb, void *user_data);
esp_err_t app_provisioning_adapter_is_provisioned(bool *provisioned);
esp_err_t app_provisioning_adapter_start(const app_provisioning_strategy_t *strategy, const char *service_name, const char *service_key);
void app_provisioning_adapter_deinit(void);

#ifdef __cplusplus
}
#endif