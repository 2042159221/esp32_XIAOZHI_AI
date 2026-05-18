#include "provisioning_service.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_prov_adapter.h"
#include "esp_prov_strategy.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "provisioning_qr_payload.h"
#include "qrcode.h"
#include "sdkconfig.h"
#include "wifi_provisioning/manager.h"
#include "wifi_sta_service.h"

static const char *TAG = "prov_service";

enum {
    WIFI_GOT_IP_BIT = BIT0,
    PROV_STOPPED_BIT = BIT1,
};

#define PROV_FINALIZE_TASK_STACK 4096
#define PROV_FINALIZE_TASK_PRIORITY 4
#define PROV_FINALIZE_DELAY_MS 100

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
#define CONFIG_APP_PROV_STOP_DELAY_MS 500
#endif

static provisioning_service_state_t s_current_state = PROVISIONING_SERVICE_STATE_UNPROVISIONED;
static provisioning_service_config_t s_service_config;
static const esp_prov_strategy_t *s_active_strategy;
static bool s_stop_requested_by_app;
static bool s_start_business_after_stop;
static bool s_prov_deinited;
static bool s_got_ip_handler_registered;
static bool s_finalize_task_active;
static portMUX_TYPE s_finalize_task_lock = portMUX_INITIALIZER_UNLOCKED;
static EventGroupHandle_t s_lifecycle_event_group;

static void provisioning_event_cb(void *user_data, wifi_prov_cb_event_t event, void *event_data);
static void log_provisioning_info(const esp_prov_strategy_t *strategy, const char *service_name, const char *service_key);
static esp_err_t start_provisioning_service(void);
static esp_err_t switch_to_fallback_strategy(const char *reason);
static void request_provisioning_stop_now(void);
static void schedule_provisioning_finalize(void);
static void provisioning_finalize_task(void *arg);
static void finish_provisioning_stop(void);
static void on_wifi_got_ip(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
static void log_heap_state(const char *stage);
static const char *provisioning_state_name(provisioning_service_state_t state);
static void stop_residual_wifi_scan(const char *stage);
static bool lifecycle_ready_to_start_business(void);
static void maybe_start_business_after_provisioning(void);
static void set_state(provisioning_service_state_t state);
static void start_business(void);

esp_err_t provisioning_service_start(const provisioning_service_config_t *config)
{
    if (config != NULL) {
        s_service_config = *config;
    }

    s_stop_requested_by_app = false;
    s_start_business_after_stop = false;
    s_prov_deinited = false;

    if (s_lifecycle_event_group == NULL) {
        s_lifecycle_event_group = xEventGroupCreate();
        ESP_RETURN_ON_FALSE(s_lifecycle_event_group != NULL,
                            ESP_ERR_NO_MEM,
                            TAG,
                            "create provisioning lifecycle event group failed");
    }
    xEventGroupClearBits(s_lifecycle_event_group, WIFI_GOT_IP_BIT | PROV_STOPPED_BIT);

    ESP_RETURN_ON_ERROR(wifi_sta_service_init(), TAG, "wifi sta init failed");

    if (!s_got_ip_handler_registered) {
        ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT,
                                                               IP_EVENT_STA_GOT_IP,
                                                               on_wifi_got_ip,
                                                               NULL,
                                                               NULL),
                            TAG,
                            "register provisioning got-ip gate handler failed");
        s_got_ip_handler_registered = true;
    }

    s_active_strategy = esp_prov_strategy_factory_create_primary();
    ESP_RETURN_ON_FALSE(s_active_strategy != NULL, ESP_FAIL, TAG, "create provisioning strategy failed");
    ESP_LOGI(TAG, "selected provisioning strategy: %s", s_active_strategy->name);

    ESP_RETURN_ON_ERROR(esp_prov_adapter_init(s_active_strategy, provisioning_event_cb, NULL),
                        TAG,
                        "provisioning adapter init failed");

    bool provisioned = false;
    ESP_RETURN_ON_ERROR(esp_prov_adapter_is_provisioned(&provisioned), TAG, "query provisioning state failed");
    if (provisioned) {
        set_state(PROVISIONING_SERVICE_STATE_CONNECTING);

        esp_err_t err = wifi_sta_service_connect_saved();
        if (err == ESP_OK) {
            set_state(PROVISIONING_SERVICE_STATE_CONNECTED);
            s_start_business_after_stop = true;
            xEventGroupSetBits(s_lifecycle_event_group, WIFI_GOT_IP_BIT);

            log_heap_state("before provisioning stop/deinit");
            stop_residual_wifi_scan("saved wifi before provisioning deinit");
            esp_prov_adapter_deinit();
            s_prov_deinited = true;
            ESP_LOGI(TAG, "provisioning deinit complete path=saved_wifi");
            xEventGroupSetBits(s_lifecycle_event_group, PROV_STOPPED_BIT);
            ESP_LOGI(TAG, "PROV_STOPPED_BIT set, PROV_DEINITED=1");
            log_heap_state("after provisioning stopped/deinit");
            maybe_start_business_after_provisioning();
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
    s_prov_deinited = false;
    if (s_lifecycle_event_group != NULL) {
        xEventGroupClearBits(s_lifecycle_event_group, PROV_STOPPED_BIT);
    }

    set_state(PROVISIONING_SERVICE_STATE_UNPROVISIONED);
    set_state(PROVISIONING_SERVICE_STATE_PROVISIONING);
    ESP_RETURN_ON_ERROR(esp_prov_adapter_disable_auto_stop(CONFIG_APP_PROV_STOP_DELAY_MS),
                        TAG,
                        "disable provisioning auto stop failed");

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
    s_prov_deinited = false;
    if (s_lifecycle_event_group != NULL) {
        xEventGroupClearBits(s_lifecycle_event_group, PROV_STOPPED_BIT);
    }

    ESP_RETURN_ON_ERROR(esp_prov_adapter_init(s_active_strategy, provisioning_event_cb, NULL),
                        TAG,
                        "fallback provisioning adapter init failed");
    ESP_RETURN_ON_ERROR(esp_prov_adapter_disable_auto_stop(CONFIG_APP_PROV_STOP_DELAY_MS),
                        TAG,
                        "disable fallback provisioning auto stop failed");

    esp_err_t err = esp_prov_adapter_start(s_active_strategy, CONFIG_APP_PROV_SERVICE_NAME, CONFIG_APP_PROV_SERVICE_KEY);
    ESP_RETURN_ON_ERROR(err, TAG, "fallback provisioning start failed");

    set_state(PROVISIONING_SERVICE_STATE_PROVISIONING);
    ESP_LOGI(TAG, "provisioning started, service_name=%s", CONFIG_APP_PROV_SERVICE_NAME);
    log_provisioning_info(s_active_strategy, CONFIG_APP_PROV_SERVICE_NAME, CONFIG_APP_PROV_SERVICE_KEY);
    return ESP_OK;
}

static void request_provisioning_stop_now(void)
{
    s_stop_requested_by_app = true;
    log_heap_state("before provisioning stop");
    ESP_LOGI(TAG, "app requested provisioning stop before business start");
    esp_prov_adapter_stop_provisioning();
    stop_residual_wifi_scan("after provisioning stop request");
    schedule_provisioning_finalize();
}

static void schedule_provisioning_finalize(void)
{
    taskENTER_CRITICAL(&s_finalize_task_lock);
    bool already_active = s_finalize_task_active;
    if (!already_active) {
        s_finalize_task_active = true;
    }
    taskEXIT_CRITICAL(&s_finalize_task_lock);

    if (already_active) {
        return;
    }

#if CONFIG_SPIRAM
    BaseType_t created = xTaskCreatePinnedToCoreWithCaps(provisioning_finalize_task,
                                                         "prov_finalize",
                                                         PROV_FINALIZE_TASK_STACK,
                                                         NULL,
                                                         PROV_FINALIZE_TASK_PRIORITY,
                                                         NULL,
                                                         tskNO_AFFINITY,
                                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    BaseType_t created = xTaskCreate(provisioning_finalize_task,
                                     "prov_finalize",
                                     PROV_FINALIZE_TASK_STACK,
                                     NULL,
                                     PROV_FINALIZE_TASK_PRIORITY,
                                     NULL);
#endif
    if (created != pdPASS) {
        taskENTER_CRITICAL(&s_finalize_task_lock);
        s_finalize_task_active = false;
        taskEXIT_CRITICAL(&s_finalize_task_lock);
        ESP_LOGE(TAG, "create provisioning finalize task failed");
    }
}

static void provisioning_finalize_task(void *arg)
{
    (void)arg;

    vTaskDelay(pdMS_TO_TICKS(PROV_FINALIZE_DELAY_MS));
    finish_provisioning_stop();

    taskENTER_CRITICAL(&s_finalize_task_lock);
    s_finalize_task_active = false;
    taskEXIT_CRITICAL(&s_finalize_task_lock);

#if CONFIG_SPIRAM
    vTaskDeleteWithCaps(NULL);
#else
    vTaskDelete(NULL);
#endif
}

static void finish_provisioning_stop(void)
{
    if (!s_prov_deinited) {
        stop_residual_wifi_scan("before provisioning deinit");
        esp_prov_adapter_deinit();
        s_prov_deinited = true;
        ESP_LOGI(TAG, "provisioning deinit complete path=finalize");
    } else {
        ESP_LOGI(TAG, "provisioning deinit complete path=already_deinited");
    }

    if (s_lifecycle_event_group != NULL) {
        xEventGroupSetBits(s_lifecycle_event_group, PROV_STOPPED_BIT);
    }
    ESP_LOGI(TAG, "PROV_STOPPED_BIT set, PROV_DEINITED=1");
    log_heap_state("after provisioning stopped/deinit");

    if (s_stop_requested_by_app) {
        s_stop_requested_by_app = false;
        ESP_LOGI(TAG, "provisioning stopped after app request");
    }

    maybe_start_business_after_provisioning();
}

static void on_wifi_got_ip(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base != IP_EVENT || event_id != IP_EVENT_STA_GOT_IP || s_lifecycle_event_group == NULL) {
        return;
    }

    xEventGroupSetBits(s_lifecycle_event_group, WIFI_GOT_IP_BIT);
    ESP_LOGI(TAG, "WIFI_GOT_IP_BIT set");
    maybe_start_business_after_provisioning();
}

static void log_heap_state(const char *stage)
{
    ESP_LOGI(TAG,
             "%s free heap=%u minimum free heap=%u internal free=%u internal largest free block=%u spiram free=%u spiram largest free block=%u",
             stage,
             (unsigned int)esp_get_free_heap_size(),
             (unsigned int)esp_get_minimum_free_heap_size(),
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

static const char *provisioning_state_name(provisioning_service_state_t state)
{
    switch (state) {
    case PROVISIONING_SERVICE_STATE_UNPROVISIONED:
        return "UNPROVISIONED";
    case PROVISIONING_SERVICE_STATE_PROVISIONING:
        return "PROVISIONING";
    case PROVISIONING_SERVICE_STATE_CRED_RECEIVED:
        return "CRED_RECEIVED";
    case PROVISIONING_SERVICE_STATE_CONNECTING:
        return "CONNECTING";
    case PROVISIONING_SERVICE_STATE_CONNECTED:
        return "CONNECTED";
    case PROVISIONING_SERVICE_STATE_FAILED:
        return "FAILED";
    case PROVISIONING_SERVICE_STATE_BUSINESS_STARTED:
        return "BUSINESS_STARTED";
    default:
        return "UNKNOWN";
    }
}

static void stop_residual_wifi_scan(const char *stage)
{
    esp_err_t err = esp_wifi_scan_stop();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "residual Wi-Fi scan stopped stage=%s", stage != NULL ? stage : "<unknown>");
        return;
    }

    if (err == ESP_ERR_WIFI_NOT_INIT || err == ESP_ERR_WIFI_NOT_STARTED || err == ESP_ERR_WIFI_STATE) {
        ESP_LOGD(TAG, "no residual Wi-Fi scan to stop stage=%s err=%s", stage != NULL ? stage : "<unknown>", esp_err_to_name(err));
        return;
    }

    ESP_LOGW(TAG, "stop residual Wi-Fi scan failed stage=%s err=%s", stage != NULL ? stage : "<unknown>", esp_err_to_name(err));
}

static bool lifecycle_ready_to_start_business(void)
{
    if (s_lifecycle_event_group == NULL || !s_prov_deinited) {
        return false;
    }

    EventBits_t bits = xEventGroupGetBits(s_lifecycle_event_group);
    return (bits & (WIFI_GOT_IP_BIT | PROV_STOPPED_BIT)) == (WIFI_GOT_IP_BIT | PROV_STOPPED_BIT);
}

static void maybe_start_business_after_provisioning(void)
{
    if (!s_start_business_after_stop || s_current_state == PROVISIONING_SERVICE_STATE_BUSINESS_STARTED) {
        return;
    }

    if (wifi_sta_service_is_connected() && s_lifecycle_event_group != NULL) {
        xEventGroupSetBits(s_lifecycle_event_group, WIFI_GOT_IP_BIT);
    }

    if (!lifecycle_ready_to_start_business()) {
        EventBits_t bits = s_lifecycle_event_group != NULL ? xEventGroupGetBits(s_lifecycle_event_group) : 0;
        ESP_LOGI(TAG,
                 "business gate waiting: got_ip=%d prov_stopped=%d prov_deinited=%d",
                 (bits & WIFI_GOT_IP_BIT) != 0,
                 (bits & PROV_STOPPED_BIT) != 0,
                 s_prov_deinited);
        return;
    }

    s_start_business_after_stop = false;
    EventBits_t bits = xEventGroupGetBits(s_lifecycle_event_group);
    ESP_LOGI(TAG,
             "business start gate passed: got_ip=%d prov_stopped=%d prov_deinited=%d",
             (bits & WIFI_GOT_IP_BIT) != 0,
             (bits & PROV_STOPPED_BIT) != 0,
             s_prov_deinited);
    start_business();
}

static void provisioning_event_cb(void *user_data, wifi_prov_cb_event_t event, void *event_data)
{
    (void)user_data;
    (void)event_data;

    switch (event) {
    case WIFI_PROV_CRED_RECV:
        ESP_LOGI(TAG, "WIFI_PROV_CRED_RECV");
        set_state(PROVISIONING_SERVICE_STATE_CRED_RECEIVED);
        set_state(PROVISIONING_SERVICE_STATE_CONNECTING);
        break;

    case WIFI_PROV_CRED_FAIL:
        ESP_LOGW(TAG, "WIFI_PROV_CRED_FAIL");
        set_state(PROVISIONING_SERVICE_STATE_FAILED);
        ESP_LOGW(TAG, "wifi provisioning credential failed");
        ESP_LOGW(TAG, "keep provisioning active for credential retry");
        break;

    case WIFI_PROV_CRED_SUCCESS:
        ESP_LOGI(TAG, "WIFI_PROV_CRED_SUCCESS");
        set_state(PROVISIONING_SERVICE_STATE_CONNECTED);
        s_start_business_after_stop = true;
        if (wifi_sta_service_is_connected() && s_lifecycle_event_group != NULL) {
            xEventGroupSetBits(s_lifecycle_event_group, WIFI_GOT_IP_BIT);
        }
        request_provisioning_stop_now();
        break;

    case WIFI_PROV_END:
        ESP_LOGI(TAG, "WIFI_PROV_END");
        schedule_provisioning_finalize();
        break;

    default:
        break;
    }
}

static void set_state(provisioning_service_state_t state)
{
    if (s_current_state != state) {
        ESP_LOGI(TAG,
                 "provisioning state %s -> %s",
                 provisioning_state_name(s_current_state),
                 provisioning_state_name(state));
    }
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

    log_heap_state("before stage1 start");
    set_state(PROVISIONING_SERVICE_STATE_BUSINESS_STARTED);
    if (s_service_config.business_start_cb != NULL) {
        s_service_config.business_start_cb(s_service_config.user_ctx);
    }
}
