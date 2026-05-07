#include "app_provisioning_manager.h"

#include <string.h>

#include "app_provisioning_adapter.h"
#include "app_provisioning_strategy.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "qrcode.h"
#include "sdkconfig.h"
#include "wifi_provisioning/manager.h"
#include "xiaozhi_wifi_sta.h"

static const char *TAG = "prov_manager";

#ifndef CONFIG_APP_PROV_SERVICE_NAME
#define CONFIG_APP_PROV_SERVICE_NAME "XIAOZHI_PROV"
#endif

#ifndef CONFIG_APP_PROV_SERVICE_KEY
#define CONFIG_APP_PROV_SERVICE_KEY "xiaozhi123"
#endif

#ifndef CONFIG_APP_PROV_POP
#define CONFIG_APP_PROV_POP "abcd1234"
#endif

static app_provisioning_state_t current_state = APP_PROVISIONING_STATE_UNPROVISIONED;
static app_provisioning_manager_config_t manager_config;
static const app_provisioning_strategy_t *active_strategy;

static void provisioning_event_cb(void *user_data, wifi_prov_cb_event_t event, void *event_data);
static void log_provisioning_info(const app_provisioning_strategy_t *strategy, const char *service_name, const char *service_key);
static void print_provisioning_qrcode(const app_provisioning_strategy_t *strategy, const char *service_name);
static esp_err_t start_provisioning_service(void);
static void set_state(app_provisioning_state_t state);
static void start_business(void);

esp_err_t app_provisioning_manager_start(const app_provisioning_manager_config_t *config)
{
    if (config != NULL) {
        manager_config = *config;
    }

    ESP_RETURN_ON_ERROR(xiaozhi_wifi_sta_init(), TAG, "wifi sta init failed");

    active_strategy = app_provisioning_strategy_factory_create();
    ESP_RETURN_ON_FALSE(active_strategy != NULL, ESP_FAIL, TAG, "create provisioning strategy failed");
    ESP_LOGI(TAG, "selected provisioning strategy: %s", active_strategy->name);

    ESP_RETURN_ON_ERROR(app_provisioning_adapter_init(active_strategy, provisioning_event_cb, NULL), TAG, "provisioning adapter init failed");

    bool provisioned = false;
    ESP_RETURN_ON_ERROR(app_provisioning_adapter_is_provisioned(&provisioned), TAG, "query provisioning state failed");
    if (provisioned) {
        set_state(APP_PROVISIONING_STATE_CONNECTING);
        esp_err_t err = xiaozhi_wifi_sta_connect_saved();
        if (err == ESP_OK) {
            set_state(APP_PROVISIONING_STATE_CONNECTED);
            start_business();
            app_provisioning_adapter_deinit();
            return ESP_OK;
        }

        ESP_LOGW(TAG, "saved WiFi connection failed: %s", esp_err_to_name(err));
        ESP_LOGW(TAG, "clear stale WiFi credentials and enter %s provisioning", active_strategy->name);
        err = xiaozhi_wifi_sta_disconnect();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "disconnect stale WiFi failed: %s", esp_err_to_name(err));
        }
        ESP_RETURN_ON_ERROR(wifi_prov_mgr_reset_provisioning(), TAG, "reset stale provisioning data failed");
    }

    return start_provisioning_service();
}

app_provisioning_state_t app_provisioning_manager_get_state(void)
{
    return current_state;
}

static esp_err_t start_provisioning_service(void)
{
    set_state(APP_PROVISIONING_STATE_UNPROVISIONED);
    set_state(APP_PROVISIONING_STATE_PROVISIONING);
    ESP_RETURN_ON_ERROR(
        app_provisioning_adapter_start(active_strategy, CONFIG_APP_PROV_SERVICE_NAME, CONFIG_APP_PROV_SERVICE_KEY),
        TAG,
        "start provisioning failed");

    ESP_LOGI(TAG, "provisioning started, service_name=%s", CONFIG_APP_PROV_SERVICE_NAME);
    log_provisioning_info(active_strategy, CONFIG_APP_PROV_SERVICE_NAME, CONFIG_APP_PROV_SERVICE_KEY);
    return ESP_OK;
}

esp_err_t app_provisioning_manager_reset_and_restart(void)
{
    ESP_LOGW(TAG, "========== PROVISIONING RESET START ==========");
    ESP_LOGW(TAG, "disconnecting WiFi and clearing saved credentials");

    esp_err_t err = xiaozhi_wifi_sta_disconnect();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "disconnect wifi before reset failed: %s", esp_err_to_name(err));
    }

    err = wifi_prov_mgr_reset_provisioning();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "reset provisioning config failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGW(TAG, "provisioning data cleared successfully");
    ESP_LOGW(TAG, "restart now; next boot should start BLE provisioning");
    ESP_LOGW(TAG, "=============================================");
    esp_restart();
    return ESP_OK;
}

static void provisioning_event_cb(void *user_data, wifi_prov_cb_event_t event, void *event_data)
{
    (void)user_data;

    switch (event) {
    case WIFI_PROV_CRED_RECV:
        set_state(APP_PROVISIONING_STATE_CRED_RECEIVED);
        set_state(APP_PROVISIONING_STATE_CONNECTING);
        break;
    case WIFI_PROV_CRED_FAIL: {
        set_state(APP_PROVISIONING_STATE_FAILED);
        ESP_LOGW(TAG, "wifi provisioning credential failed");
        esp_err_t err = wifi_prov_mgr_reset_sm_state_on_failure();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "reset provisioning state machine failed: %s", esp_err_to_name(err));
        }
        set_state(APP_PROVISIONING_STATE_PROVISIONING);
        break;
    }
    case WIFI_PROV_CRED_SUCCESS:
        set_state(APP_PROVISIONING_STATE_CONNECTED);
        start_business();
        break;
    case WIFI_PROV_END:
        app_provisioning_adapter_deinit();
        break;
    default:
        break;
    }
}

static void set_state(app_provisioning_state_t state)
{
    current_state = state;
    if (manager_config.state_cb != NULL) {
        manager_config.state_cb(state, manager_config.user_ctx);
    }
}

static void log_provisioning_info(const app_provisioning_strategy_t *strategy, const char *service_name, const char *service_key)
{
    const char *effective_service_key = service_key;
    if (strategy->transport != NULL && strcmp(strategy->transport, "ble") == 0) {
        effective_service_key = "";
    }

    ESP_LOGI(TAG, "========== XIAOZHI PROVISIONING ==========");
    ESP_LOGI(TAG, "transport: %s", strategy->name);
    ESP_LOGI(TAG, "service name: %s", service_name);
    ESP_LOGI(TAG, "security: Security 1");
    ESP_LOGI(TAG, "proof of possession: %s", CONFIG_APP_PROV_POP);
    if (effective_service_key[0] != '\0') {
        ESP_LOGI(TAG, "service key: %s", effective_service_key);
    }
    ESP_LOGI(TAG, "scan this payload with Espressif Provisioning app:");
    ESP_LOGI(TAG,
             "{\"ver\":\"%s\",\"name\":\"%s\",\"pop\":\"%s\",\"transport\":\"%s\",\"security\":\"1\",\"capabilities\":[\"%s\"]}",
             strategy->qr_version,
             service_name,
             CONFIG_APP_PROV_POP,
             strategy->transport,
             strategy->capabilities);
    print_provisioning_qrcode(strategy, service_name);
    ESP_LOGI(TAG, "==========================================");
}

static void print_provisioning_qrcode(const app_provisioning_strategy_t *strategy, const char *service_name)
{
    char payload[256];
    int written = snprintf(payload,
                           sizeof(payload),
                           "{\"ver\":\"%s\",\"name\":\"%s\",\"pop\":\"%s\",\"transport\":\"%s\",\"security\":\"1\",\"capabilities\":[\"%s\"]}",
                           strategy->qr_version,
                           service_name,
                           CONFIG_APP_PROV_POP,
                           strategy->transport,
                           strategy->capabilities);
    if (written < 0 || written >= sizeof(payload)) {
        ESP_LOGW(TAG, "provisioning QR payload is too long");
        return;
    }

    ESP_LOGI(TAG, "scan QR code below with Espressif Provisioning app:");
    esp_qrcode_config_t qrcode_config = ESP_QRCODE_CONFIG_DEFAULT();
    qrcode_config.max_qrcode_version = 10;
    qrcode_config.qrcode_ecc_level = ESP_QRCODE_ECC_LOW;

    esp_err_t err = esp_qrcode_generate(&qrcode_config, payload);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "generate provisioning QR code failed: %s", esp_err_to_name(err));
    }
}

static void start_business(void)
{
    if (current_state == APP_PROVISIONING_STATE_BUSINESS_STARTED) {
        return;
    }

    set_state(APP_PROVISIONING_STATE_BUSINESS_STARTED);
    if (manager_config.business_start_cb != NULL) {
        manager_config.business_start_cb(manager_config.user_ctx);
    }
}