#include "app_provisioning_manager.h"

#include <string.h>

#include "app_provisioning_adapter.h"
#include "app_provisioning_strategy.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
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

#ifndef CONFIG_APP_PROV_STOP_DELAY_MS
#define CONFIG_APP_PROV_STOP_DELAY_MS 10000
#endif

static app_provisioning_state_t current_state = APP_PROVISIONING_STATE_UNPROVISIONED;
static app_provisioning_manager_config_t manager_config;
static const app_provisioning_strategy_t *active_strategy;
static bool restart_provisioning_after_stop;
static bool stop_requested_by_app;
static esp_timer_handle_t provisioning_stop_timer;

static void provisioning_event_cb(void *user_data, wifi_prov_cb_event_t event, void *event_data);
static void log_provisioning_info(const app_provisioning_strategy_t *strategy, const char *service_name, const char *service_key);
static void print_provisioning_qrcode(const app_provisioning_strategy_t *strategy, const char *service_name);
static esp_err_t start_provisioning_service(void);
static esp_err_t switch_to_fallback_strategy(const char *reason);
static esp_err_t restart_active_provisioning_service(const char *reason);
static esp_err_t schedule_provisioning_stop(void);
static void stop_provisioning_timer_cb(void *arg);
static void set_state(app_provisioning_state_t state);
static void start_business(void);

esp_err_t app_provisioning_manager_start(const app_provisioning_manager_config_t *config)
{
    if (config != NULL) {
        manager_config = *config;
    }

    restart_provisioning_after_stop = false;
    stop_requested_by_app = false;

    if (provisioning_stop_timer == NULL) {
        const esp_timer_create_args_t timer_args = {
            .callback = stop_provisioning_timer_cb,
            .arg = NULL,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "prov_stop_tm",
            .skip_unhandled_events = true,
        };
        ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &provisioning_stop_timer), TAG, "create provisioning stop timer failed");
    }

    ESP_RETURN_ON_ERROR(xiaozhi_wifi_sta_init(), TAG, "wifi sta init failed");

    active_strategy = app_provisioning_strategy_factory_create_primary();
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
    ESP_RETURN_ON_ERROR(wifi_prov_mgr_disable_auto_stop(CONFIG_APP_PROV_STOP_DELAY_MS), TAG, "disable provisioning auto stop failed");
    esp_err_t err = app_provisioning_adapter_start(active_strategy, CONFIG_APP_PROV_SERVICE_NAME, CONFIG_APP_PROV_SERVICE_KEY);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "start %s provisioning failed: %s", active_strategy->name, esp_err_to_name(err));
        return switch_to_fallback_strategy("preferred provisioning start failed");
    }

    ESP_LOGI(TAG, "provisioning started, service_name=%s", CONFIG_APP_PROV_SERVICE_NAME);
    log_provisioning_info(active_strategy, CONFIG_APP_PROV_SERVICE_NAME, CONFIG_APP_PROV_SERVICE_KEY);
    return ESP_OK;
}

static esp_err_t switch_to_fallback_strategy(const char *reason)
{
    const app_provisioning_strategy_t *fallback_strategy = app_provisioning_strategy_factory_create_fallback(active_strategy);
    if (fallback_strategy == NULL) {
        return ESP_FAIL;
    }

    ESP_LOGW(TAG, "fallback to %s provisioning: %s", fallback_strategy->name, reason);

    app_provisioning_adapter_deinit();
    active_strategy = fallback_strategy;
    stop_requested_by_app = false;

    ESP_RETURN_ON_ERROR(app_provisioning_adapter_init(active_strategy, provisioning_event_cb, NULL), TAG, "fallback provisioning adapter init failed");
    ESP_RETURN_ON_ERROR(wifi_prov_mgr_disable_auto_stop(CONFIG_APP_PROV_STOP_DELAY_MS), TAG, "disable fallback provisioning auto stop failed");

    esp_err_t err = active_strategy->start(active_strategy, CONFIG_APP_PROV_SERVICE_NAME, CONFIG_APP_PROV_SERVICE_KEY);
    ESP_RETURN_ON_ERROR(err, TAG, "fallback provisioning start failed");

    ESP_LOGI(TAG, "provisioning started, service_name=%s", CONFIG_APP_PROV_SERVICE_NAME);
    log_provisioning_info(active_strategy, CONFIG_APP_PROV_SERVICE_NAME, CONFIG_APP_PROV_SERVICE_KEY);
    return ESP_OK;
}

static esp_err_t restart_active_provisioning_service(const char *reason)
{
    ESP_LOGW(TAG, "restart %s provisioning service: %s", active_strategy->name, reason);

    stop_requested_by_app = false;
    ESP_RETURN_ON_ERROR(app_provisioning_adapter_init(active_strategy, provisioning_event_cb, NULL), TAG, "re-init provisioning adapter failed");
    ESP_RETURN_ON_ERROR(wifi_prov_mgr_disable_auto_stop(CONFIG_APP_PROV_STOP_DELAY_MS), TAG, "disable provisioning auto stop before restart failed");

    esp_err_t err = active_strategy->start(active_strategy, CONFIG_APP_PROV_SERVICE_NAME, CONFIG_APP_PROV_SERVICE_KEY);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "restart %s provisioning failed: %s", active_strategy->name, esp_err_to_name(err));
        return switch_to_fallback_strategy("restart preferred provisioning failed");
    }

    set_state(APP_PROVISIONING_STATE_PROVISIONING);
    ESP_LOGI(TAG, "provisioning restarted, service_name=%s", CONFIG_APP_PROV_SERVICE_NAME);
    log_provisioning_info(active_strategy, CONFIG_APP_PROV_SERVICE_NAME, CONFIG_APP_PROV_SERVICE_KEY);
    return ESP_OK;
}

static esp_err_t schedule_provisioning_stop(void)
{
    if (provisioning_stop_timer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    stop_requested_by_app = true;
    esp_err_t err = esp_timer_stop(provisioning_stop_timer);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    ESP_LOGI(TAG, "schedule provisioning stop in %d ms", CONFIG_APP_PROV_STOP_DELAY_MS);
    return esp_timer_start_once(provisioning_stop_timer, CONFIG_APP_PROV_STOP_DELAY_MS * 1000ULL);
}

static void stop_provisioning_timer_cb(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "app requested provisioning stop after delay");
    wifi_prov_mgr_stop_provisioning();
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
        restart_provisioning_after_stop = true;
        wifi_prov_mgr_stop_provisioning();
        break;
    }
    case WIFI_PROV_CRED_SUCCESS:
        set_state(APP_PROVISIONING_STATE_CONNECTED);
        start_business();
        {
            esp_err_t err = schedule_provisioning_stop();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "schedule provisioning stop failed: %s", esp_err_to_name(err));
            }
        }
        break;
    case WIFI_PROV_END:
        app_provisioning_adapter_deinit();
        if (stop_requested_by_app) {
            stop_requested_by_app = false;
            ESP_LOGI(TAG, "provisioning stopped after app-managed delay");
        }
        if (restart_provisioning_after_stop) {
            restart_provisioning_after_stop = false;
            esp_err_t err = restart_active_provisioning_service("credential failure requested a clean restart");
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "restart provisioning service failed: %s", esp_err_to_name(err));
            }
        }
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