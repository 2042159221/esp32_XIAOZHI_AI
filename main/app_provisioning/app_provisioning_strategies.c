#include "app_provisioning_strategy.h"

#include "sdkconfig.h"
#include "wifi_provisioning/scheme_ble.h"
#include "wifi_provisioning/scheme_softap.h"

#ifndef CONFIG_APP_PROV_TRANSPORT_BLE
#define CONFIG_APP_PROV_TRANSPORT_BLE 1
#endif

#ifndef CONFIG_APP_PROV_POP
#define CONFIG_APP_PROV_POP "abcd1234"
#endif

static esp_err_t strategy_start(const app_provisioning_strategy_t *strategy, const char *service_name, const char *service_key)
{
    (void)strategy;
    const char *pop = CONFIG_APP_PROV_POP;
    return wifi_prov_mgr_start_provisioning(WIFI_PROV_SECURITY_1, pop, service_name, service_key);
}

static wifi_prov_mgr_config_t create_ble_manager_config(wifi_prov_cb_func_t event_cb, void *user_data)
{
    wifi_prov_mgr_config_t config = {
        .scheme = wifi_prov_scheme_ble,
        .scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM,
        .app_event_handler = {
            .event_cb = event_cb,
            .user_data = user_data,
        },
    };

    return config;
}

static wifi_prov_mgr_config_t create_softap_manager_config(wifi_prov_cb_func_t event_cb, void *user_data)
{
    (void)event_cb;
    (void)user_data;
    return (wifi_prov_mgr_config_t) {0};
}

static const app_provisioning_strategy_t ble_strategy = {
    .name = "BLE",
    .transport = "ble",
    .qr_version = "v1",
    .capabilities = "wifi_scan",
    .create_manager_config = create_ble_manager_config,
    .start = strategy_start,
};

const app_provisioning_strategy_t *app_provisioning_strategy_factory_create(void)
{
    return &ble_strategy;
}