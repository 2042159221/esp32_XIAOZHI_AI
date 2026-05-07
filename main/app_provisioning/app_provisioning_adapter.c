#include "app_provisioning_adapter.h"

#include "esp_check.h"
#include "wifi_provisioning/manager.h"

static const char *TAG = "prov_adapter";

esp_err_t app_provisioning_adapter_init(const app_provisioning_strategy_t *strategy, wifi_prov_cb_func_t event_cb, void *user_data)
{
    ESP_RETURN_ON_FALSE(strategy != NULL, ESP_ERR_INVALID_ARG, TAG, "strategy is null");

    wifi_prov_mgr_config_t manager_config = strategy->create_manager_config(event_cb, user_data);
    return wifi_prov_mgr_init(manager_config);
}

esp_err_t app_provisioning_adapter_is_provisioned(bool *provisioned)
{
    ESP_RETURN_ON_FALSE(provisioned != NULL, ESP_ERR_INVALID_ARG, TAG, "provisioned is null");
    return wifi_prov_mgr_is_provisioned(provisioned);
}

esp_err_t app_provisioning_adapter_start(const app_provisioning_strategy_t *strategy, const char *service_name, const char *service_key)
{
    ESP_RETURN_ON_FALSE(strategy != NULL, ESP_ERR_INVALID_ARG, TAG, "strategy is null");
    return strategy->start(strategy, service_name, service_key);
}

void app_provisioning_adapter_deinit(void)
{
    wifi_prov_mgr_deinit();
}