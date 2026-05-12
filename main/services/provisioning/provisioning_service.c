#include "provisioning_service.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_prov_adapter.h"
#include "esp_prov_strategy.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "provisioning_qr_payload.h"
#include "qrcode.h"
#include "sdkconfig.h"
#include "wifi_provisioning/manager.h"
#include "wifi_sta_service.h"

static const char *TAG = "prov_service";

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

static provisioning_service_state_t s_current_state = PROVISIONING_SERVICE_STATE_UNPROVISIONED;
static provisioning_service_config_t s_service_config;
static const esp_prov_strategy_t *s_active_strategy;
static bool s_restart_provisioning_after_stop;
static bool s_stop_requested_by_app;
static esp_timer_handle_t s_provisioning_stop_timer;

static void provisioning_event_cb(void *user_data, wifi_prov_cb_event_t event, void *event_data);
static void log_provisioning_info(const esp_prov_strategy_t *strategy, const char *service_name, const char *service_key);
static esp_err_t start_provisioning_service(void);
static esp_err_t switch_to_fallback_strategy(const char *reason);
static esp_err_t restart_active_provisioning_service(const char *reason);
static esp_err_t schedule_provisioning_stop(void);
static void stop_provisioning_timer_cb(void *arg);
static void set_state(provisioning_service_state_t state);
static void start_business(void);

esp_err_t provisioning_service_start(const provisioning_service_config_t *config)
{
    if (config != NULL) {
        s_service_config = *config;
    }

    s_restart_provisioning_after_stop = false;
    s_stop_requested_by_app = false;

    if (s_provisioning_stop_timer == NULL) {
        const esp_timer_create_args_t timer_args = {
            .callback = stop_provisioning_timer_cb,
            .arg = NULL,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "prov_stop_tm",
            .skip_unhandled_events = true,
        };
        ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_provisioning_stop_timer), TAG, "create provisioning stop timer failed");
    }

    ESP_RETURN_ON_ERROR(wifi_sta_service_init(), TAG, "wifi sta init failed");

    s_active_strategy = esp_prov_strategy_factory_create_primary();
    ESP_RETURN_ON_FALSE(s_active_strategy != NULL, ESP_FAIL, TAG, "create provisioning strategy failed");
    ESP_LOGI(TAG, "selected provisioning strategy: %s", s_active_strategy->name);

    ESP_RETURN_ON_ERROR(esp_prov_adapter_init(s_active_strategy, provisioning_event_cb, NULL), TAG, "provisioning adapter init failed");

    bool provisioned = false;
    ESP_RETURN_ON_ERROR(esp_prov_adapter_is_provisioned(&provisioned), TAG, "query provisioning state failed");
    if (provisioned) {
        set_state(PROVISIONING_SERVICE_STATE_CONNECTING);
        esp_err_t err = wifi_sta_service_connect_saved();
        if (err == ESP_OK) {
            set_state(PROVISIONING_SERVICE_STATE_CONNECTED);
            start_business();
            esp_prov_adapter_deinit();
            return ESP_OK;
        }

        ESP_LOGW(TAG, "saved WiFi connection failed: %s", esp_err_to_name(err));
        ESP_LOGW(TAG, "clear stale WiFi credentials and enter %s provisioning", s_active_strategy->name);
        err = wifi_sta_service_disconnect();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "disconnect stale WiFi failed: %s", esp_err_to_name(err));
        }
        ESP_RETURN_ON_ERROR(esp_prov_adapter_reset_provisioning(), TAG, "reset stale provisioning data failed");
    }

    return start_provisioning_service();
}

provisioning_service_state_t provisioning_service_get_state(void)
{
    return s_current_state;
}

esp_err_t provisioning_service_reset_and_restart(void)
{
    ESP_LOGW(TAG, "========== PROVISIONING RESET START ==========");
    ESP_LOGW(TAG, "disconnecting WiFi and clearing saved credentials");

    esp_err_t err = wifi_sta_service_disconnect();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "disconnect wifi before reset failed: %s", esp_err_to_name(err));
    }

    err = esp_prov_adapter_reset_provisioning();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "reset provisioning config failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGW(TAG, "provisioning data cleared successfully");
    ESP_LOGW(TAG, "restart now; next boot should start configured provisioning");
    ESP_LOGW(TAG, "=============================================");
    esp_restart();
    return ESP_OK;
}

static esp_err_t start_provisioning_service(void)
{
    set_state(PROVISIONING_SERVICE_STATE_UNPROVISIONED);
    set_state(PROVISIONING_SERVICE_STATE_PROVISIONING);
    ESP_RETURN_ON_ERROR(esp_prov_adapter_disable_auto_stop(CONFIG_APP_PROV_STOP_DELAY_MS), TAG, "disable provisioning auto stop failed");

    esp_err_t err = esp_prov_adapter_start(s_active_strategy, CONFIG_APP_PROV_SERVICE_NAME, CONFIG_APP_PROV_SERVICE_KEY);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "start %s provisioning failed: %s", s_active_strategy->name, esp_err_to_name(err));
        return switch_to_fallback_strategy("preferred provisioning start failed");
    }

    ESP_LOGI(TAG, "provisioning started, service_name=%s", CONFIG_APP_PROV_SERVICE_NAME);
    log_provisioning_info(s_active_strategy, CONFIG_APP_PROV_SERVICE_NAME, CONFIG_APP_PROV_SERVICE_KEY);
    return ESP_OK;
}

static esp_err_t switch_to_fallback_strategy(const char *reason)
{
    const esp_prov_strategy_t *fallback_strategy = esp_prov_strategy_factory_create_fallback(s_active_strategy);
    if (fallback_strategy == NULL) {
        return ESP_FAIL;
    }

    ESP_LOGW(TAG, "fallback to %s provisioning: %s", fallback_strategy->name, reason);

    esp_prov_adapter_deinit();
    s_active_strategy = fallback_strategy;
    s_stop_requested_by_app = false;

    ESP_RETURN_ON_ERROR(esp_prov_adapter_init(s_active_strategy, provisioning_event_cb, NULL), TAG, "fallback provisioning adapter init failed");
    ESP_RETURN_ON_ERROR(esp_prov_adapter_disable_auto_stop(CONFIG_APP_PROV_STOP_DELAY_MS), TAG, "disable fallback provisioning auto stop failed");

    esp_err_t err = esp_prov_adapter_start(s_active_strategy, CONFIG_APP_PROV_SERVICE_NAME, CONFIG_APP_PROV_SERVICE_KEY);
    ESP_RETURN_ON_ERROR(err, TAG, "fallback provisioning start failed");

    set_state(PROVISIONING_SERVICE_STATE_PROVISIONING);
    ESP_LOGI(TAG, "provisioning started, service_name=%s", CONFIG_APP_PROV_SERVICE_NAME);
    log_provisioning_info(s_active_strategy, CONFIG_APP_PROV_SERVICE_NAME, CONFIG_APP_PROV_SERVICE_KEY);
    return ESP_OK;
}

static esp_err_t restart_active_provisioning_service(const char *reason)
{
    ESP_LOGW(TAG, "restart %s provisioning service: %s", s_active_strategy->name, reason);

    s_stop_requested_by_app = false;
    ESP_RETURN_ON_ERROR(esp_prov_adapter_init(s_active_strategy, provisioning_event_cb, NULL), TAG, "re-init provisioning adapter failed");
    ESP_RETURN_ON_ERROR(esp_prov_adapter_disable_auto_stop(CONFIG_APP_PROV_STOP_DELAY_MS), TAG, "disable provisioning auto stop before restart failed");

    esp_err_t err = esp_prov_adapter_start(s_active_strategy, CONFIG_APP_PROV_SERVICE_NAME, CONFIG_APP_PROV_SERVICE_KEY);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "restart %s provisioning failed: %s", s_active_strategy->name, esp_err_to_name(err));
        return switch_to_fallback_strategy("restart preferred provisioning failed");
    }

    set_state(PROVISIONING_SERVICE_STATE_PROVISIONING);
    ESP_LOGI(TAG, "provisioning restarted, service_name=%s", CONFIG_APP_PROV_SERVICE_NAME);
    log_provisioning_info(s_active_strategy, CONFIG_APP_PROV_SERVICE_NAME, CONFIG_APP_PROV_SERVICE_KEY);
    return ESP_OK;
}

static esp_err_t schedule_provisioning_stop(void)
{
    if (s_provisioning_stop_timer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_stop_requested_by_app = true;
    esp_err_t err = esp_timer_stop(s_provisioning_stop_timer);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    ESP_LOGI(TAG, "schedule provisioning stop in %d ms", CONFIG_APP_PROV_STOP_DELAY_MS);
    return esp_timer_start_once(s_provisioning_stop_timer, CONFIG_APP_PROV_STOP_DELAY_MS * 1000ULL);
}

static void stop_provisioning_timer_cb(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "app requested provisioning stop after delay");
    esp_prov_adapter_stop_provisioning();
}

static void provisioning_event_cb(void *user_data, wifi_prov_cb_event_t event, void *event_data)
{
    (void)user_data;
    (void)event_data;

    switch (event) {
    case WIFI_PROV_CRED_RECV:
        set_state(PROVISIONING_SERVICE_STATE_CRED_RECEIVED);
        set_state(PROVISIONING_SERVICE_STATE_CONNECTING);
        break;
    case WIFI_PROV_CRED_FAIL:
        set_state(PROVISIONING_SERVICE_STATE_FAILED);
        ESP_LOGW(TAG, "wifi provisioning credential failed");
        s_restart_provisioning_after_stop = true;
        esp_prov_adapter_stop_provisioning();
        break;
    case WIFI_PROV_CRED_SUCCESS:
        set_state(PROVISIONING_SERVICE_STATE_CONNECTED);
        start_business();
        {
            esp_err_t err = schedule_provisioning_stop();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "schedule provisioning stop failed: %s", esp_err_to_name(err));
            }
        }
        break;
    case WIFI_PROV_END:
        esp_prov_adapter_deinit();
        if (s_stop_requested_by_app) {
            s_stop_requested_by_app = false;
            ESP_LOGI(TAG, "provisioning stopped after app-managed delay");
        }
        if (s_restart_provisioning_after_stop) {
            s_restart_provisioning_after_stop = false;
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

static void set_state(provisioning_service_state_t state)
{
    s_current_state = state;
    if (s_service_config.state_cb != NULL) {
        s_service_config.state_cb(state, s_service_config.user_ctx);
    }
}

static void log_provisioning_info(const esp_prov_strategy_t *strategy, const char *service_name, const char *service_key)
{
    char *payload = NULL;
    esp_err_t err = provisioning_qr_payload_alloc(strategy, service_name, &payload);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "build provisioning QR payload failed: %s", esp_err_to_name(err));
        return;
    }

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
    ESP_LOGI(TAG, "%s", payload);

    if (s_service_config.qrcode_cb != NULL) {
        s_service_config.qrcode_cb(payload, s_service_config.user_ctx);
    }

    if (strategy->transport == NULL || strcmp(strategy->transport, "ble") != 0) {
        ESP_LOGI(TAG, "scan QR code below with Espressif Provisioning app:");
        esp_qrcode_config_t qrcode_config = ESP_QRCODE_CONFIG_DEFAULT();
        qrcode_config.max_qrcode_version = 10;
        qrcode_config.qrcode_ecc_level = ESP_QRCODE_ECC_LOW;

        err = esp_qrcode_generate(&qrcode_config, payload);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "generate provisioning QR code failed: %s", esp_err_to_name(err));
        }
    }

    ESP_LOGI(TAG, "==========================================");
    free(payload);
}

static void start_business(void)
{
    if (s_current_state == PROVISIONING_SERVICE_STATE_BUSINESS_STARTED) {
        return;
    }

    set_state(PROVISIONING_SERVICE_STATE_BUSINESS_STARTED);
    if (s_service_config.business_start_cb != NULL) {
        s_service_config.business_start_cb(s_service_config.user_ctx);
    }
}
