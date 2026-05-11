#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_prov_strategy.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t esp_prov_adapter_init(const esp_prov_strategy_t *strategy, wifi_prov_cb_func_t event_cb, void *user_data);
esp_err_t esp_prov_adapter_is_provisioned(bool *provisioned);
esp_err_t esp_prov_adapter_start(const esp_prov_strategy_t *strategy, const char *service_name, const char *service_key);
esp_err_t esp_prov_adapter_disable_auto_stop(uint32_t stop_delay_ms);
esp_err_t esp_prov_adapter_reset_provisioning(void);
void esp_prov_adapter_stop_provisioning(void);
void esp_prov_adapter_deinit(void);

#ifdef __cplusplus
}
#endif
