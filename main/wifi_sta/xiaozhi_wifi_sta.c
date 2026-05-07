#include "xiaozhi_wifi_sta.h"

#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "xiaozhi_wifi_sta";

enum {
    WIFI_CONNECTED_BIT = BIT0,
    WIFI_FAIL_BIT = BIT1,
    WIFI_DEFAULT_MAX_RETRY = 5,
};

static EventGroupHandle_t wifi_event_group;
static esp_netif_t *wifi_sta_netif;
static uint8_t retry_count;
static uint8_t max_retry = WIFI_DEFAULT_MAX_RETRY;
static bool wifi_initialized;
static bool wifi_connected;

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
static esp_err_t init_nvs(void);
static esp_err_t copy_wifi_field(uint8_t *dest, size_t dest_size, const char *src, const char *field_name);

#ifndef CONFIG_ESP_WIFI_SSID
#define CONFIG_ESP_WIFI_SSID ""
#endif

#ifndef CONFIG_ESP_WIFI_PASSWORD
#define CONFIG_ESP_WIFI_PASSWORD ""
#endif

#ifndef CONFIG_ESP_WIFI_PW_ID
#define CONFIG_ESP_WIFI_PW_ID ""
#endif

#ifndef CONFIG_ESP_MAXIMUM_RETRY
#define CONFIG_ESP_MAXIMUM_RETRY WIFI_DEFAULT_MAX_RETRY
#endif

#ifndef CONFIG_ESP_WIFI_AUTH_OPEN
#define CONFIG_ESP_WIFI_AUTH_OPEN 0
#endif

#ifndef CONFIG_ESP_WIFI_AUTH_WEP
#define CONFIG_ESP_WIFI_AUTH_WEP 0
#endif

#ifndef CONFIG_ESP_WIFI_AUTH_WPA_PSK
#define CONFIG_ESP_WIFI_AUTH_WPA_PSK 0
#endif

#ifndef CONFIG_ESP_WIFI_AUTH_WPA2_PSK
#define CONFIG_ESP_WIFI_AUTH_WPA2_PSK 1
#endif

#ifndef CONFIG_ESP_WIFI_AUTH_WPA_WPA2_PSK
#define CONFIG_ESP_WIFI_AUTH_WPA_WPA2_PSK 0
#endif

#ifndef CONFIG_ESP_WIFI_AUTH_WPA3_PSK
#define CONFIG_ESP_WIFI_AUTH_WPA3_PSK 0
#endif

#ifndef CONFIG_ESP_WIFI_AUTH_WPA2_WPA3_PSK
#define CONFIG_ESP_WIFI_AUTH_WPA2_WPA3_PSK 0
#endif

#ifndef CONFIG_ESP_WIFI_AUTH_WAPI_PSK
#define CONFIG_ESP_WIFI_AUTH_WAPI_PSK 0
#endif

#ifndef CONFIG_ESP_WPA3_SAE_PWE_HUNT_AND_PECK
#define CONFIG_ESP_WPA3_SAE_PWE_HUNT_AND_PECK 0
#endif

#ifndef CONFIG_ESP_WPA3_SAE_PWE_HASH_TO_ELEMENT
#define CONFIG_ESP_WPA3_SAE_PWE_HASH_TO_ELEMENT 0
#endif

#if CONFIG_ESP_WIFI_AUTH_OPEN
#define XIAOZHI_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_OPEN
#elif CONFIG_ESP_WIFI_AUTH_WEP
#define XIAOZHI_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WEP
#elif CONFIG_ESP_WIFI_AUTH_WPA_PSK
#define XIAOZHI_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA2_PSK
#define XIAOZHI_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA_WPA2_PSK
#define XIAOZHI_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_WPA2_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA3_PSK
#define XIAOZHI_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA3_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA2_WPA3_PSK
#define XIAOZHI_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_WPA3_PSK
#elif CONFIG_ESP_WIFI_AUTH_WAPI_PSK
#define XIAOZHI_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WAPI_PSK
#else
#define XIAOZHI_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK
#endif

#if CONFIG_ESP_WPA3_SAE_PWE_HUNT_AND_PECK
#define XIAOZHI_WIFI_SAE_PWE_MODE WPA3_SAE_PWE_HUNT_AND_PECK
#elif CONFIG_ESP_WPA3_SAE_PWE_HASH_TO_ELEMENT
#define XIAOZHI_WIFI_SAE_PWE_MODE WPA3_SAE_PWE_HASH_TO_ELEMENT
#else
#define XIAOZHI_WIFI_SAE_PWE_MODE WPA3_SAE_PWE_BOTH
#endif

esp_err_t xiaozhi_wifi_sta_init(void)
{
    if (wifi_initialized) {
        return ESP_OK;
    }
    //初始化FLASH：持久化存储对应的信息
    ESP_RETURN_ON_ERROR(init_nvs(), TAG, "init nvs failed");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "init esp netif failed");

    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(err, TAG, "create default event loop failed");
    }

    wifi_sta_netif = esp_netif_create_default_wifi_sta();
    ESP_RETURN_ON_FALSE(wifi_sta_netif != NULL, ESP_FAIL, TAG, "create default wifi sta failed");

    wifi_event_group = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(wifi_event_group != NULL, ESP_ERR_NO_MEM, TAG, "create wifi event group failed");

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_config), TAG, "init wifi failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL), TAG, "register wifi event handler failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL), TAG, "register ip event handler failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set wifi mode sta failed");

    wifi_initialized = true;

    return ESP_OK;
}

esp_err_t xiaozhi_wifi_sta_connect_default(void)
{
    const xiaozhi_wifi_sta_config_t config = {
        .ssid = CONFIG_ESP_WIFI_SSID,
        .password = CONFIG_ESP_WIFI_PASSWORD,
        .max_retry = CONFIG_ESP_MAXIMUM_RETRY,
        .authmode = XIAOZHI_WIFI_SCAN_AUTH_MODE_THRESHOLD,
    };

    return xiaozhi_wifi_sta_connect(&config);
}

esp_err_t xiaozhi_wifi_sta_connect_saved(void)
{
    ESP_RETURN_ON_ERROR(xiaozhi_wifi_sta_init(), TAG, "wifi sta init failed");

    max_retry = CONFIG_ESP_MAXIMUM_RETRY;
    retry_count = 0;
    wifi_connected = false;
    xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start wifi failed");

    EventBits_t bits = xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));
    ESP_RETURN_ON_FALSE((bits & WIFI_CONNECTED_BIT) != 0, ESP_FAIL, TAG, "connect to saved wifi failed");

    return ESP_OK;
}

esp_err_t xiaozhi_wifi_sta_connect(const xiaozhi_wifi_sta_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "config is null");
    ESP_RETURN_ON_FALSE(config->ssid[0] != '\0', ESP_ERR_INVALID_ARG, TAG, "ssid is empty");

    ESP_RETURN_ON_ERROR(xiaozhi_wifi_sta_init(), TAG, "wifi sta init failed");

    wifi_config_t wifi_config = {0};
    ESP_RETURN_ON_ERROR(copy_wifi_field(wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), config->ssid, "ssid"), TAG, "copy ssid failed");
    ESP_RETURN_ON_ERROR(copy_wifi_field(wifi_config.sta.password, sizeof(wifi_config.sta.password), config->password, "password"), TAG, "copy password failed");

    wifi_config.sta.threshold.authmode = config->authmode;
    wifi_config.sta.sae_pwe_h2e = XIAOZHI_WIFI_SAE_PWE_MODE;
    ESP_RETURN_ON_ERROR(copy_wifi_field(wifi_config.sta.sae_h2e_identifier, sizeof(wifi_config.sta.sae_h2e_identifier), CONFIG_ESP_WIFI_PW_ID, "sae password identifier"), TAG, "copy sae password identifier failed");
    max_retry = config->max_retry > 0 ? config->max_retry : WIFI_DEFAULT_MAX_RETRY;
    retry_count = 0;
    wifi_connected = false;
    xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG, "set wifi config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start wifi failed");

    EventBits_t bits = xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));
    ESP_RETURN_ON_FALSE((bits & WIFI_CONNECTED_BIT) != 0, ESP_FAIL, TAG, "connect to wifi failed");

    return ESP_OK;
}

esp_err_t xiaozhi_wifi_sta_disconnect(void)
{
    ESP_RETURN_ON_FALSE(wifi_initialized, ESP_ERR_INVALID_STATE, TAG, "wifi is not initialized");

    wifi_connected = false;
    xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_RETURN_ON_ERROR(err, TAG, "disconnect wifi failed");
    }

    err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_RETURN_ON_ERROR(err, TAG, "stop wifi failed");
    }

    return ESP_OK;
}

bool xiaozhi_wifi_sta_is_connected(void)
{
    return wifi_connected;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = (const wifi_event_sta_disconnected_t *)event_data;
        wifi_connected = false;

        if (retry_count < max_retry) {
            retry_count++;
            esp_wifi_connect();
            ESP_LOGW(TAG, "wifi disconnected, reason=%d, retry %u/%u", event->reason, retry_count, max_retry);
            return;
        }

        xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
        ESP_LOGE(TAG, "wifi connection failed, reason=%d", event->reason);
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        retry_count = 0;
        wifi_connected = true;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "\033[1;32m========== WIFI STA CONNECTED ==========\033[0m");
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase nvs failed");
        err = nvs_flash_init();
    }

    return err;
}

static esp_err_t copy_wifi_field(uint8_t *dest, size_t dest_size, const char *src, const char *field_name)
{
    size_t src_len = strlen(src);
    ESP_RETURN_ON_FALSE(src_len < dest_size, ESP_ERR_INVALID_ARG, TAG, "%s is too long", field_name);

    memcpy(dest, src, src_len);
    dest[src_len] = '\0';

    return ESP_OK;
}