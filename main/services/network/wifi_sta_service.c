#include "wifi_sta_service.h"

#include <stdbool.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_platform_init.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "sdkconfig.h"

static const char *TAG = "wifi_sta_service";

enum {
    WIFI_CONNECTED_BIT = BIT0,
    WIFI_FAIL_BIT = BIT1,
    WIFI_DEFAULT_MAX_RETRY = 5,
};

#define WIFI_STA_SERVICE_COUNTRY_CODE "CN"
#define WIFI_STA_SERVICE_IEEE80211D_ENABLED false

static EventGroupHandle_t s_wifi_event_group;
static esp_netif_t *s_wifi_sta_netif;
static uint8_t s_retry_count;
static uint8_t s_max_retry = WIFI_DEFAULT_MAX_RETRY;
static bool s_wifi_initialized;
static bool s_wifi_connected;

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
static esp_err_t configure_wifi_country(void);
static void log_wifi_country(const char *label, const wifi_country_t *country);
static const char *wifi_country_policy_name(wifi_country_policy_t policy);
static esp_err_t copy_wifi_field(uint8_t *dest, size_t dest_size, const char *src, const char *field_name);
static void log_dns_server(esp_netif_t *netif, esp_netif_dns_type_t type, const char *label);
static void log_network_info(esp_netif_t *netif, const esp_netif_ip_info_t *ip_info);

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
#define WIFI_STA_SERVICE_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_OPEN
#elif CONFIG_ESP_WIFI_AUTH_WEP
#define WIFI_STA_SERVICE_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WEP
#elif CONFIG_ESP_WIFI_AUTH_WPA_PSK
#define WIFI_STA_SERVICE_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA2_PSK
#define WIFI_STA_SERVICE_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA_WPA2_PSK
#define WIFI_STA_SERVICE_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_WPA2_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA3_PSK
#define WIFI_STA_SERVICE_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA3_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA2_WPA3_PSK
#define WIFI_STA_SERVICE_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_WPA3_PSK
#elif CONFIG_ESP_WIFI_AUTH_WAPI_PSK
#define WIFI_STA_SERVICE_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WAPI_PSK
#else
#define WIFI_STA_SERVICE_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK
#endif

#if CONFIG_ESP_WPA3_SAE_PWE_HUNT_AND_PECK
#define WIFI_STA_SERVICE_SAE_PWE_MODE WPA3_SAE_PWE_HUNT_AND_PECK
#elif CONFIG_ESP_WPA3_SAE_PWE_HASH_TO_ELEMENT
#define WIFI_STA_SERVICE_SAE_PWE_MODE WPA3_SAE_PWE_HASH_TO_ELEMENT
#else
#define WIFI_STA_SERVICE_SAE_PWE_MODE WPA3_SAE_PWE_BOTH
#endif

esp_err_t wifi_sta_service_init(void)
{
    if (s_wifi_initialized) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(esp_platform_init(), TAG, "init platform before wifi failed");

    s_wifi_sta_netif = esp_netif_create_default_wifi_sta();
    ESP_RETURN_ON_FALSE(s_wifi_sta_netif != NULL, ESP_FAIL, TAG, "create default wifi sta failed");

    s_wifi_event_group = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_wifi_event_group != NULL, ESP_ERR_NO_MEM, TAG, "create wifi event group failed");

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_config), TAG, "init wifi failed");
    ESP_RETURN_ON_ERROR(configure_wifi_country(), TAG, "configure wifi country failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL), TAG, "register wifi event handler failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL), TAG, "register ip event handler failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set wifi mode sta failed");

    s_wifi_initialized = true;
    return ESP_OK;
}

esp_err_t wifi_sta_service_connect_default(void)
{
    const wifi_sta_service_config_t config = {
        .ssid = CONFIG_ESP_WIFI_SSID,
        .password = CONFIG_ESP_WIFI_PASSWORD,
        .max_retry = CONFIG_ESP_MAXIMUM_RETRY,
        .authmode = WIFI_STA_SERVICE_SCAN_AUTH_MODE_THRESHOLD,
    };

    return wifi_sta_service_connect(&config);
}

esp_err_t wifi_sta_service_connect_saved(void)
{
    ESP_RETURN_ON_ERROR(wifi_sta_service_init(), TAG, "wifi sta init failed");

    s_max_retry = CONFIG_ESP_MAXIMUM_RETRY;
    s_retry_count = 0;
    s_wifi_connected = false;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start wifi failed");

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));
    ESP_RETURN_ON_FALSE((bits & WIFI_CONNECTED_BIT) != 0, ESP_FAIL, TAG, "connect to saved wifi failed");

    return ESP_OK;
}

esp_err_t wifi_sta_service_connect(const wifi_sta_service_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "config is null");
    ESP_RETURN_ON_FALSE(config->ssid[0] != '\0', ESP_ERR_INVALID_ARG, TAG, "ssid is empty");

    ESP_RETURN_ON_ERROR(wifi_sta_service_init(), TAG, "wifi sta init failed");

    wifi_config_t wifi_config = {0};
    ESP_RETURN_ON_ERROR(copy_wifi_field(wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), config->ssid, "ssid"), TAG, "copy ssid failed");
    ESP_RETURN_ON_ERROR(copy_wifi_field(wifi_config.sta.password, sizeof(wifi_config.sta.password), config->password, "password"), TAG, "copy password failed");

    wifi_config.sta.threshold.authmode = config->authmode;
    wifi_config.sta.sae_pwe_h2e = WIFI_STA_SERVICE_SAE_PWE_MODE;
    ESP_RETURN_ON_ERROR(copy_wifi_field(wifi_config.sta.sae_h2e_identifier,
                                        sizeof(wifi_config.sta.sae_h2e_identifier),
                                        CONFIG_ESP_WIFI_PW_ID,
                                        "sae password identifier"),
                        TAG,
                        "copy sae password identifier failed");

    s_max_retry = config->max_retry > 0 ? config->max_retry : WIFI_DEFAULT_MAX_RETRY;
    s_retry_count = 0;
    s_wifi_connected = false;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG, "set wifi config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start wifi failed");

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));
    ESP_RETURN_ON_FALSE((bits & WIFI_CONNECTED_BIT) != 0, ESP_FAIL, TAG, "connect to wifi failed");

    return ESP_OK;
}

esp_err_t wifi_sta_service_wait_connected(uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(s_wifi_initialized, ESP_ERR_INVALID_STATE, TAG, "wifi is not initialized");
    ESP_RETURN_ON_FALSE(s_wifi_event_group != NULL, ESP_ERR_INVALID_STATE, TAG, "wifi event group is not initialized");

    if (s_wifi_connected) {
        return ESP_OK;
    }

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
    ESP_RETURN_ON_FALSE((bits & WIFI_CONNECTED_BIT) != 0,
                        (bits & WIFI_FAIL_BIT) != 0 ? ESP_FAIL : ESP_ERR_TIMEOUT,
                        TAG,
                        "wait wifi connected timeout, timeout=%u ms",
                        (unsigned int)timeout_ms);

    return ESP_OK;
}

esp_err_t wifi_sta_service_disconnect(void)
{
    ESP_RETURN_ON_FALSE(s_wifi_initialized, ESP_ERR_INVALID_STATE, TAG, "wifi is not initialized");

    s_wifi_connected = false;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

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

bool wifi_sta_service_is_connected(void)
{
    return s_wifi_connected;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = (const wifi_event_sta_disconnected_t *)event_data;
        s_wifi_connected = false;

        if (s_retry_count < s_max_retry) {
            s_retry_count++;
            esp_wifi_connect();
            ESP_LOGW(TAG, "wifi disconnected, reason=%d, retry %u/%u", event->reason, s_retry_count, s_max_retry);
            return;
        }

        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        ESP_LOGE(TAG, "wifi connection failed, reason=%d", event->reason);
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        s_retry_count = 0;
        s_wifi_connected = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "========== WIFI STA CONNECTED ==========");
        log_network_info(event->esp_netif != NULL ? event->esp_netif : s_wifi_sta_netif, &event->ip_info);
    }
}

static esp_err_t configure_wifi_country(void)
{
    wifi_country_t current = {0};
    esp_err_t err = esp_wifi_get_country(&current);
    if (err == ESP_OK) {
        log_wifi_country("wifi country before configure", &current);
    } else {
        ESP_LOGW(TAG, "get wifi country before configure failed: %s", esp_err_to_name(err));
    }

    const bool already_configured = err == ESP_OK &&
                                    current.cc[0] == WIFI_STA_SERVICE_COUNTRY_CODE[0] &&
                                    current.cc[1] == WIFI_STA_SERVICE_COUNTRY_CODE[1] &&
                                    current.schan == 1 &&
                                    current.nchan == 13 &&
                                    current.policy == WIFI_COUNTRY_POLICY_MANUAL;
    if (!already_configured) {
        ESP_RETURN_ON_ERROR(esp_wifi_set_country_code(WIFI_STA_SERVICE_COUNTRY_CODE,
                                                      WIFI_STA_SERVICE_IEEE80211D_ENABLED),
                            TAG,
                            "set wifi country code failed");
    }

    wifi_country_t configured = {0};
    ESP_RETURN_ON_ERROR(esp_wifi_get_country(&configured), TAG, "get configured wifi country failed");
    log_wifi_country(already_configured ? "wifi country already configured" : "wifi country configured", &configured);
    return ESP_OK;
}

static void log_wifi_country(const char *label, const wifi_country_t *country)
{
    if (country == NULL) {
        return;
    }

    ESP_LOGI(TAG,
             "%s cc=%c%c schan=%u nchan=%u max_tx_power=%d policy=%s ieee80211d=%s",
             label,
             country->cc[0],
             country->cc[1],
             (unsigned int)country->schan,
             (unsigned int)country->nchan,
             (int)country->max_tx_power,
             wifi_country_policy_name(country->policy),
             WIFI_STA_SERVICE_IEEE80211D_ENABLED ? "enabled" : "disabled");
}

static const char *wifi_country_policy_name(wifi_country_policy_t policy)
{
    switch (policy) {
    case WIFI_COUNTRY_POLICY_AUTO:
        return "AUTO";
    case WIFI_COUNTRY_POLICY_MANUAL:
        return "MANUAL";
    default:
        return "UNKNOWN";
    }
}

static esp_err_t copy_wifi_field(uint8_t *dest, size_t dest_size, const char *src, const char *field_name)
{
    size_t src_len = strlen(src);
    ESP_RETURN_ON_FALSE(src_len < dest_size, ESP_ERR_INVALID_ARG, TAG, "%s is too long", field_name);

    memcpy(dest, src, src_len);
    dest[src_len] = '\0';

    return ESP_OK;
}

static void log_network_info(esp_netif_t *netif, const esp_netif_ip_info_t *ip_info)
{
    if (ip_info != NULL) {
        ESP_LOGI(TAG,
                 "got ip: " IPSTR ", netmask: " IPSTR ", gw: " IPSTR,
                 IP2STR(&ip_info->ip),
                 IP2STR(&ip_info->netmask),
                 IP2STR(&ip_info->gw));
    }

    if (netif == NULL) {
        ESP_LOGW(TAG, "wifi sta netif is null, cannot print DNS");
        return;
    }

    log_dns_server(netif, ESP_NETIF_DNS_MAIN, "main");
    log_dns_server(netif, ESP_NETIF_DNS_BACKUP, "backup");
    log_dns_server(netif, ESP_NETIF_DNS_FALLBACK, "fallback");
}

static void log_dns_server(esp_netif_t *netif, esp_netif_dns_type_t type, const char *label)
{
    esp_netif_dns_info_t dns = {0};
    esp_err_t err = esp_netif_get_dns_info(netif, type, &dns);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "dns %s unavailable: %s", label, esp_err_to_name(err));
        return;
    }

    if (dns.ip.type == ESP_IPADDR_TYPE_V6) {
        ESP_LOGI(TAG, "dns %s: " IPV6STR, label, IPV62STR(dns.ip.u_addr.ip6));
        return;
    }

    ESP_LOGI(TAG, "dns %s: " IPSTR, label, IP2STR(&dns.ip.u_addr.ip4));
}
