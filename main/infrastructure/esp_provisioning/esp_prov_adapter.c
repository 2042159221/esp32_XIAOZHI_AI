#include "esp_prov_adapter.h"

#include "esp_check.h"
#include "wifi_provisioning/manager.h"

static const char *TAG = "esp_prov_adapter";

esp_err_t esp_prov_adapter_init(const esp_prov_strategy_t *strategy, wifi_prov_cb_func_t event_cb, void *user_data)
{
    ESP_RETURN_ON_FALSE(strategy != NULL, ESP_ERR_INVALID_ARG, TAG, "strategy is null");

    wifi_prov_mgr_config_t manager_config = strategy->create_manager_config(event_cb, user_data);
    return wifi_prov_mgr_init(manager_config);
}

esp_err_t esp_prov_adapter_is_provisioned(bool *provisioned)
{
    ESP_RETURN_ON_FALSE(provisioned != NULL, ESP_ERR_INVALID_ARG, TAG, "provisioned is null");
    return wifi_prov_mgr_is_provisioned(provisioned);
}

esp_err_t esp_prov_adapter_start(const esp_prov_strategy_t *strategy, const char *service_name, const char *service_key)
{
    ESP_RETURN_ON_FALSE(strategy != NULL, ESP_ERR_INVALID_ARG, TAG, "strategy is null");
    return strategy->start(strategy, service_name, service_key);
}

esp_err_t esp_prov_adapter_disable_auto_stop(uint32_t stop_delay_ms)
{
    return wifi_prov_mgr_disable_auto_stop(stop_delay_ms);
}

esp_err_t esp_prov_adapter_reset_provisioning(void)
{
    return wifi_prov_mgr_reset_provisioning();
}

void esp_prov_adapter_stop_provisioning(void)
{
    wifi_prov_mgr_stop_provisioning();
}

void esp_prov_adapter_deinit(void)
{
    wifi_prov_mgr_deinit();
}
