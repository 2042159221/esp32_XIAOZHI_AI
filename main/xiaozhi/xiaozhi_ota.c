#include "xiaozhi_ota.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "wifi_sta_service.h"
#include "xiaozhi_device.h"
#include "xiaozhi_handle.h"

static const char *TAG = "xiaozhi_ota";

#define XIAOZHI_HTTP_RESPONSE_MAX_LEN (32 * 1024)
#define XIAOZHI_OTA_HOST_MAX_LEN 128
#define XIAOZHI_OTA_HTTP_RX_BUFFER_SIZE 512
#define XIAOZHI_OTA_HTTP_TX_BUFFER_SIZE 512

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} xiaozhi_http_response_t;

static esp_err_t wait_for_network_ready(uint32_t timeout_ms);
static esp_err_t log_sta_network_snapshot(bool *dns_ready);
static esp_err_t extract_hostname_from_url(const char *url, char *host, size_t host_size);
static esp_err_t resolve_hostname_once(const char *host);
static const char *gai_error_name(int error);
static void delay_between_dns_retries(int attempt, int max_attempts);
static int get_dns_retry_count(void);
static int get_dns_retry_delay_ms(void);
static void log_ota_heap_state(int attempt, int max_attempts, const char *stage);

static void *xiaozhi_malloc_prefer_spiram(size_t size)
{
#if CONFIG_SPIRAM
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr != NULL) {
        return ptr;
    }
#endif

    return heap_caps_malloc(size, MALLOC_CAP_8BIT);
}

static void response_buffer_free(xiaozhi_http_response_t *response)
{
    if (response == NULL) {
        return;
    }

    heap_caps_free(response->data);
    response->data = NULL;
    response->len = 0;
    response->cap = 0;
}

static esp_err_t wait_for_network_ready(uint32_t timeout_ms)
{
    esp_err_t err = wifi_sta_service_wait_connected(timeout_ms);
    ESP_RETURN_ON_ERROR(err, TAG, "wifi is not ready for ota");

    bool dns_ready = false;
    err = log_sta_network_snapshot(&dns_ready);
    ESP_RETURN_ON_ERROR(err, TAG, "network snapshot failed");
    ESP_RETURN_ON_FALSE(dns_ready, ESP_ERR_INVALID_STATE, TAG, "DNS server is not configured");

    return ESP_OK;
}

static esp_err_t log_sta_network_snapshot(bool *dns_ready)
{
    if (dns_ready != NULL) {
        *dns_ready = false;
    }

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    ESP_RETURN_ON_FALSE(netif != NULL, ESP_ERR_NOT_FOUND, TAG, "wifi sta netif not found");

    esp_netif_ip_info_t ip_info = {0};
    ESP_RETURN_ON_ERROR(esp_netif_get_ip_info(netif, &ip_info), TAG, "get sta ip info failed");
    ESP_RETURN_ON_FALSE(ip_info.ip.addr != 0, ESP_ERR_INVALID_STATE, TAG, "sta ip is 0.0.0.0");

    ESP_LOGI(TAG,
             "network ready: ip=" IPSTR " netmask=" IPSTR " gw=" IPSTR,
             IP2STR(&ip_info.ip),
             IP2STR(&ip_info.netmask),
             IP2STR(&ip_info.gw));

    const esp_netif_dns_type_t dns_types[] = {
        ESP_NETIF_DNS_MAIN,
        ESP_NETIF_DNS_BACKUP,
        ESP_NETIF_DNS_FALLBACK,
    };
    const char *dns_labels[] = {
        "main",
        "backup",
        "fallback",
    };

    bool found_dns = false;
    for (size_t i = 0; i < sizeof(dns_types) / sizeof(dns_types[0]); ++i) {
        esp_netif_dns_info_t dns = {0};
        esp_err_t err = esp_netif_get_dns_info(netif, dns_types[i], &dns);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "dns %s unavailable: %s", dns_labels[i], esp_err_to_name(err));
            continue;
        }

        if (dns.ip.type == ESP_IPADDR_TYPE_V6) {
            ESP_LOGI(TAG, "dns %s=" IPV6STR, dns_labels[i], IPV62STR(dns.ip.u_addr.ip6));
            if (dns.ip.u_addr.ip6.addr[0] != 0 || dns.ip.u_addr.ip6.addr[1] != 0 ||
                dns.ip.u_addr.ip6.addr[2] != 0 || dns.ip.u_addr.ip6.addr[3] != 0) {
                found_dns = true;
            }
            continue;
        }

        ESP_LOGI(TAG, "dns %s=" IPSTR, dns_labels[i], IP2STR(&dns.ip.u_addr.ip4));
        if (dns.ip.u_addr.ip4.addr != 0) {
            found_dns = true;
        }
    }

    if (dns_ready != NULL) {
        *dns_ready = found_dns;
    }

    return ESP_OK;
}

static esp_err_t extract_hostname_from_url(const char *url, char *host, size_t host_size)
{
    ESP_RETURN_ON_FALSE(url != NULL && url[0] != '\0', ESP_ERR_INVALID_ARG, TAG, "url is empty");
    ESP_RETURN_ON_FALSE(host != NULL && host_size > 0, ESP_ERR_INVALID_ARG, TAG, "host output is invalid");

    const char *scheme = strstr(url, "://");
    ESP_RETURN_ON_FALSE(scheme != NULL, ESP_ERR_INVALID_ARG, TAG, "ota url missing scheme: %s", url);

    const char *host_start = scheme + 3;
    ESP_RETURN_ON_FALSE(host_start[0] != '\0', ESP_ERR_INVALID_ARG, TAG, "ota url missing host: %s", url);

    const char *host_end = host_start;
    while (*host_end != '\0' && *host_end != '/' && *host_end != '?' && *host_end != '#') {
        host_end++;
    }

    const char *port_sep = NULL;
    if (*host_start == '[') {
        const char *ipv6_end = strchr(host_start, ']');
        ESP_RETURN_ON_FALSE(ipv6_end != NULL && ipv6_end < host_end, ESP_ERR_INVALID_ARG, TAG, "invalid IPv6 host in ota url");
        host_start++;
        port_sep = ipv6_end + 1 < host_end && ipv6_end[1] == ':' ? ipv6_end + 1 : NULL;
        host_end = ipv6_end;
    } else {
        port_sep = memchr(host_start, ':', (size_t)(host_end - host_start));
    }

    if (port_sep != NULL) {
        host_end = port_sep;
    }

    size_t host_len = (size_t)(host_end - host_start);
    ESP_RETURN_ON_FALSE(host_len > 0 && host_len < host_size,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "ota url host is invalid or too long, len=%u",
                        (unsigned int)host_len);

    memcpy(host, host_start, host_len);
    host[host_len] = '\0';
    return ESP_OK;
}

static esp_err_t resolve_hostname_once(const char *host)
{
    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *addrinfo = NULL;
    int gai_err = getaddrinfo(host, NULL, &hints, &addrinfo);
    if (gai_err != 0 || addrinfo == NULL) {
        ESP_LOGW(TAG, "DNS resolve failed: host=%s getaddrinfo=%d(%s) addrinfo=%p", host, gai_err, gai_error_name(gai_err), addrinfo);
        if (addrinfo != NULL) {
            freeaddrinfo(addrinfo);
        }
        return ESP_ERR_NOT_FOUND;
    }

    const struct sockaddr_in *addr = (const struct sockaddr_in *)addrinfo->ai_addr;
    if (addr != NULL) {
        esp_ip4_addr_t ip = {
            .addr = addr->sin_addr.s_addr,
        };
        ESP_LOGI(TAG, "DNS resolve OK: %s -> " IPSTR, host, IP2STR(&ip));
    } else {
        ESP_LOGI(TAG, "DNS resolve OK: %s", host);
    }

    freeaddrinfo(addrinfo);
    return ESP_OK;
}

static const char *gai_error_name(int error)
{
    switch (error) {
    case 0:
        return "OK";
    case EAI_NONAME:
        return "EAI_NONAME";
    case EAI_SERVICE:
        return "EAI_SERVICE";
    case EAI_FAIL:
        return "EAI_FAIL";
    case EAI_MEMORY:
        return "EAI_MEMORY";
    case EAI_FAMILY:
        return "EAI_FAMILY";
    default:
        return "UNKNOWN";
    }
}

static void delay_between_dns_retries(int attempt, int max_attempts)
{
    if (attempt + 1 >= max_attempts) {
        return;
    }

    int delay_ms = get_dns_retry_delay_ms();
    ESP_LOGW(TAG, "retry OTA after DNS/connect failure in %d ms, attempt %d/%d", delay_ms, attempt + 2, max_attempts);
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
}

static int get_dns_retry_count(void)
{
    return CONFIG_XIAOZHI_OTA_DNS_RETRY_COUNT > 0 ? CONFIG_XIAOZHI_OTA_DNS_RETRY_COUNT : 1;
}

static int get_dns_retry_delay_ms(void)
{
    return CONFIG_XIAOZHI_OTA_DNS_RETRY_DELAY_MS > 0 ? CONFIG_XIAOZHI_OTA_DNS_RETRY_DELAY_MS : 1000;
}

static void log_ota_heap_state(int attempt, int max_attempts, const char *stage)
{
    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    ESP_LOGI(TAG,
             "OTA heap attempt=%d/%d stage=%s free heap=%lu minimum free heap=%lu internal free=%u internal largest free block=%u",
             attempt + 1,
             max_attempts,
             stage,
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)esp_get_minimum_free_heap_size(),
             (unsigned int)internal_free,
             (unsigned int)internal_largest);
}

static esp_err_t response_buffer_append(xiaozhi_http_response_t *response, const char *data, size_t data_len)
{
    ESP_RETURN_ON_FALSE(response != NULL, ESP_ERR_INVALID_ARG, TAG, "response buffer is null");
    ESP_RETURN_ON_FALSE(data != NULL || data_len == 0, ESP_ERR_INVALID_ARG, TAG, "http data is null");

    if (data_len == 0) {
        return ESP_OK;
    }

    ESP_RETURN_ON_FALSE(data_len <= XIAOZHI_HTTP_RESPONSE_MAX_LEN && response->len <= XIAOZHI_HTTP_RESPONSE_MAX_LEN - data_len,
                        ESP_ERR_NO_MEM,
                        TAG,
                        "http response too large");

    size_t needed = response->len + data_len + 1;
    if (needed > response->cap) {
        size_t next_cap = response->cap == 0 ? 512 : response->cap;
        while (next_cap < needed) {
            next_cap *= 2;
        }

        if (next_cap > XIAOZHI_HTTP_RESPONSE_MAX_LEN + 1) {
            next_cap = XIAOZHI_HTTP_RESPONSE_MAX_LEN + 1;
        }

        char *next = (char *)xiaozhi_malloc_prefer_spiram(next_cap);
        ESP_RETURN_ON_FALSE(next != NULL, ESP_ERR_NO_MEM, TAG, "alloc http response buffer failed, size=%u", (unsigned int)next_cap);
        if (response->data != NULL && response->len > 0) {
            memcpy(next, response->data, response->len);
        }
        heap_caps_free(response->data);
        response->data = next;
        response->cap = next_cap;
    }

    memcpy(response->data + response->len, data, data_len);
    response->len += data_len;
    response->data[response->len] = '\0';
    return ESP_OK;
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xiaozhi_http_response_t *response = (xiaozhi_http_response_t *)evt->user_data;

    switch (evt->event_id) {
    case HTTP_EVENT_ERROR:
        ESP_LOGW(TAG, "HTTP_EVENT_ERROR");
        break;
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
        break;
    case HTTP_EVENT_HEADER_SENT:
        ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
        break;
    case HTTP_EVENT_ON_HEADER:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
        break;
    case HTTP_EVENT_ON_DATA:
        if (evt->data != NULL && evt->data_len > 0) {
            esp_err_t err = response_buffer_append(response, (const char *)evt->data, (size_t)evt->data_len);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "append response data failed: %s", esp_err_to_name(err));
                return err;
            }
        }
        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
        break;
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
        break;
    case HTTP_EVENT_REDIRECT:
        ESP_LOGD(TAG, "HTTP_EVENT_REDIRECT");
        break;
    default:
        break;
    }

    return ESP_OK;
}

static void log_token_masked(const char *label, const char *token)
{
    if (token == NULL) {
        ESP_LOGI(TAG, "%s: <null>", label);
        return;
    }

    size_t len = strlen(token);
    ESP_LOGI(TAG, "%s: present, len=%u", label, (unsigned int)len);
}

static char *format_user_agent(void)
{
    const char *board_name = xiaozhi_device_get_board_name();
    const char *app_version = xiaozhi_device_get_app_version();
    const char *board_type = xiaozhi_device_get_board_type();

    int required = snprintf(NULL, 0, "%s/%s %s", board_name, app_version, board_type);
    if (required < 0) {
        return NULL;
    }

    char *user_agent = (char *)heap_caps_malloc((size_t)required + 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (user_agent == NULL) {
        return NULL;
    }

    int written = snprintf(user_agent, (size_t)required + 1, "%s/%s %s", board_name, app_version, board_type);
    if (written != required) {
        heap_caps_free(user_agent);
        return NULL;
    }

    return user_agent;
}

static esp_err_t add_string_to_object(cJSON *object, const char *name, const char *value)
{
    ESP_RETURN_ON_FALSE(object != NULL && name != NULL && value != NULL, ESP_ERR_INVALID_ARG, TAG, "json string arg is null");
    ESP_RETURN_ON_FALSE(cJSON_AddStringToObject(object, name, value) != NULL, ESP_ERR_NO_MEM, TAG, "add json string %s failed", name);
    return ESP_OK;
}

static esp_err_t build_request_body(char **out_body)
{
    ESP_RETURN_ON_FALSE(out_body != NULL, ESP_ERR_INVALID_ARG, TAG, "out_body is null");
    *out_body = NULL;

    char uuid[XIAOZHI_UUID_STR_LEN] = {0};
    char mac[XIAOZHI_MAC_STR_LEN] = {0};
    char ip[XIAOZHI_IPV4_STR_LEN] = {0};
    char ssid[33] = {0};
    char sha256[XIAOZHI_ELF_SHA256_MIN_STR_LEN] = {0};
    int rssi = 0;
    int channel = 0;

    ESP_RETURN_ON_ERROR(xiaozhi_device_get_or_create_uuid(uuid, sizeof(uuid)), TAG, "get uuid failed");
    ESP_RETURN_ON_ERROR(xiaozhi_device_get_mac_str(mac, sizeof(mac)), TAG, "get mac failed");
    ESP_RETURN_ON_ERROR(xiaozhi_device_get_elf_sha256(sha256, sizeof(sha256)), TAG, "get elf sha256 failed");

    esp_err_t ip_err = xiaozhi_device_get_ip_str(ip, sizeof(ip));
    if (ip_err != ESP_OK) {
        ESP_LOGW(TAG, "get ip failed: %s", esp_err_to_name(ip_err));
        snprintf(ip, sizeof(ip), "0.0.0.0");
    }

    esp_err_t wifi_err = xiaozhi_device_get_wifi_info(ssid, sizeof(ssid), &rssi, &channel);
    if (wifi_err != ESP_OK) {
        ESP_LOGW(TAG, "get wifi info failed: %s", esp_err_to_name(wifi_err));
    }

    cJSON *root = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(root != NULL, ESP_ERR_NO_MEM, TAG, "create ota json root failed");

    esp_err_t ret = ESP_OK;
    cJSON *application = cJSON_CreateObject();
    cJSON *board = cJSON_CreateObject();
    if (application == NULL || board == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    ESP_GOTO_ON_FALSE(cJSON_AddItemToObject(root, "application", application),
                      ESP_ERR_NO_MEM,
                      cleanup,
                      TAG,
                      "add application object failed");
    cJSON *application_obj = application;
    application = NULL;
    ESP_GOTO_ON_FALSE(cJSON_AddItemToObject(root, "board", board), ESP_ERR_NO_MEM, cleanup, TAG, "add board object failed");
    cJSON *board_obj = board;
    board = NULL;

    ret = add_string_to_object(application_obj, "version", xiaozhi_device_get_app_version());
    if (ret != ESP_OK) {
        goto cleanup;
    }
    ret = add_string_to_object(application_obj, "elf_sha256", sha256);
    if (ret != ESP_OK) {
        goto cleanup;
    }

    ret = add_string_to_object(board_obj, "type", xiaozhi_device_get_board_type());
    if (ret != ESP_OK) {
        goto cleanup;
    }
    ret = add_string_to_object(board_obj, "name", xiaozhi_device_get_board_name());
    if (ret != ESP_OK) {
        goto cleanup;
    }
    ret = add_string_to_object(board_obj, "ssid", ssid);
    if (ret != ESP_OK) {
        goto cleanup;
    }
    ESP_GOTO_ON_FALSE(cJSON_AddNumberToObject(board_obj, "rssi", rssi) != NULL, ESP_ERR_NO_MEM, cleanup, TAG, "add rssi failed");
    ESP_GOTO_ON_FALSE(cJSON_AddNumberToObject(board_obj, "channel", channel) != NULL, ESP_ERR_NO_MEM, cleanup, TAG, "add channel failed");
    ret = add_string_to_object(board_obj, "ip", ip);
    if (ret != ESP_OK) {
        goto cleanup;
    }
    ret = add_string_to_object(board_obj, "mac", mac);
    if (ret != ESP_OK) {
        goto cleanup;
    }

    *out_body = cJSON_PrintUnformatted(root);
    if (*out_body == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }

cleanup:
    if (application != NULL) {
        cJSON_Delete(application);
    }
    if (board != NULL) {
        cJSON_Delete(board);
    }
    cJSON_Delete(root);
    return ret;
}

static const char *get_optional_json_string(cJSON *object, const char *name)
{
    cJSON *item = cJSON_GetObjectItem(object, name);
    if (item == NULL) {
        return NULL;
    }

    if (!cJSON_IsString(item)) {
        ESP_LOGW(TAG, "json field %s is not a string", name);
        return NULL;
    }

    return item->valuestring;
}

static esp_err_t parse_ota_response(const char *response)
{
    ESP_RETURN_ON_FALSE(response != NULL && response[0] != '\0', ESP_ERR_INVALID_RESPONSE, TAG, "empty ota response");

    cJSON *root = cJSON_Parse(response);
    ESP_RETURN_ON_FALSE(root != NULL, ESP_ERR_INVALID_RESPONSE, TAG, "parse ota response failed");

    esp_err_t err = ESP_OK;
    cJSON *websocket = cJSON_GetObjectItem(root, "websocket");
    if (!cJSON_IsObject(websocket)) {
        ESP_LOGE(TAG, "missing websocket object in ota response");
        err = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }

    cJSON *url = cJSON_GetObjectItem(websocket, "url");
    cJSON *token = cJSON_GetObjectItem(websocket, "token");
    if (!cJSON_IsString(url) || !cJSON_IsString(token)) {
        ESP_LOGE(TAG, "missing websocket.url or websocket.token in ota response");
        err = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }

    err = xiaozhi_handle_set_websocket(url->valuestring, token->valuestring);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save websocket config failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    cJSON *activation = cJSON_GetObjectItem(root, "activation");
    if (activation != NULL) {
        if (!cJSON_IsObject(activation)) {
            ESP_LOGW(TAG, "activation exists but is not an object");
            err = ESP_ERR_INVALID_RESPONSE;
            goto cleanup;
        }

        const char *code = get_optional_json_string(activation, "code");
        const char *message = get_optional_json_string(activation, "message");
        const char *challenge = get_optional_json_string(activation, "challenge");
        if (code == NULL || code[0] == '\0') {
            ESP_LOGW(TAG, "activation object is present but code is missing");
        }

        err = xiaozhi_handle_set_activation(code, message, challenge);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "save activation config failed: %s", esp_err_to_name(err));
            goto cleanup;
        }
        xiaozhi_handle_set_activated(false);
        ESP_LOGI(TAG, "activation required, code=%s", code != NULL ? code : "<missing>");
    } else {
        err = xiaozhi_handle_set_activation(NULL, NULL, NULL);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "clear activation config failed: %s", esp_err_to_name(err));
            goto cleanup;
        }
        xiaozhi_handle_set_activated(true);
        ESP_LOGI(TAG, "device is activated");
    }

    ESP_LOGI(TAG, "websocket url present: %s", xiaozhi_handle_get_websocket_url() != NULL ? "yes" : "no");
    log_token_masked("websocket token", xiaozhi_handle_get_websocket_token());

cleanup:
    cJSON_Delete(root);
    return err;
}

esp_err_t xiaozhi_ota_request(const xiaozhi_ota_config_t *config)
{
    const char *ota_url = config != NULL && config->ota_url != NULL ? config->ota_url : XIAOZHI_DEFAULT_OTA_URL;
    int timeout_ms = config != NULL && config->timeout_ms > 0 ? config->timeout_ms : CONFIG_XIAOZHI_HTTP_TIMEOUT_MS;

    ESP_RETURN_ON_FALSE(ota_url != NULL && ota_url[0] != '\0', ESP_ERR_INVALID_ARG, TAG, "ota url is empty");
    ESP_RETURN_ON_ERROR(wait_for_network_ready(CONFIG_XIAOZHI_OTA_NET_READY_TIMEOUT_MS), TAG, "network is not ready");

    esp_err_t err = xiaozhi_handle_init();
    ESP_RETURN_ON_ERROR(err, TAG, "init shared handle failed");

    char uuid[XIAOZHI_UUID_STR_LEN] = {0};
    char mac[XIAOZHI_MAC_STR_LEN] = {0};
    ESP_RETURN_ON_ERROR(xiaozhi_device_get_or_create_uuid(uuid, sizeof(uuid)), TAG, "get uuid failed");
    ESP_RETURN_ON_ERROR(xiaozhi_device_get_mac_str(mac, sizeof(mac)), TAG, "get mac failed");

    char *request_body = NULL;
    err = build_request_body(&request_body);
    ESP_RETURN_ON_ERROR(err, TAG, "build ota request body failed");

    char *user_agent = format_user_agent();
    if (user_agent == NULL) {
        cJSON_free(request_body);
        return ESP_ERR_NO_MEM;
    }

    char ota_host[XIAOZHI_OTA_HOST_MAX_LEN] = {0};
    err = extract_hostname_from_url(ota_url, ota_host, sizeof(ota_host));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "parse ota host failed: %s", esp_err_to_name(err));
        goto cleanup_common;
    }

    int max_attempts = get_dns_retry_count();
    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        xiaozhi_http_response_t response = {0};
        esp_http_client_handle_t client = NULL;
        bool retry_attempt = false;

        log_ota_heap_state(attempt, max_attempts, "before http init");
        esp_http_client_config_t http_config = {
            .url = ota_url,
            .method = HTTP_METHOD_POST,
            .event_handler = http_event_handler,
            .user_data = &response,
            .timeout_ms = timeout_ms,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .buffer_size = XIAOZHI_OTA_HTTP_RX_BUFFER_SIZE,
            .buffer_size_tx = XIAOZHI_OTA_HTTP_TX_BUFFER_SIZE,
            .keep_alive_enable = false,
        };

        client = esp_http_client_init(&http_config);
        if (client == NULL) {
            err = ESP_ERR_NO_MEM;
            ESP_LOGE(TAG, "create http client failed");
            log_ota_heap_state(attempt, max_attempts, "after http init failed");
            break;
        }
        log_ota_heap_state(attempt, max_attempts, "after http init");

        ESP_LOGI(TAG, "request ota url=%s timeout=%dms attempt=%d/%d", ota_url, timeout_ms, attempt + 1, max_attempts);
        ESP_LOGI(TAG,
                 "http client buffers: rx=%d tx=%d",
                 XIAOZHI_OTA_HTTP_RX_BUFFER_SIZE,
                 XIAOZHI_OTA_HTTP_TX_BUFFER_SIZE);
        ESP_LOGI(TAG, "Client-Id(UUID)=%s", uuid);
        ESP_LOGI(TAG, "Device-Id(MAC)=%s", mac);

        esp_err_t resolve_err = resolve_hostname_once(ota_host);
        if (resolve_err != ESP_OK) {
            err = ESP_ERR_HTTP_CONNECT;
            retry_attempt = true;
            goto attempt_cleanup;
        }

        err = esp_http_client_set_header(client, "Content-Type", "application/json");
        if (err == ESP_OK) {
            err = esp_http_client_set_header(client, "User-Agent", user_agent);
        }
        if (err == ESP_OK) {
            err = esp_http_client_set_header(client, "Device-Id", mac);
        }
        if (err == ESP_OK) {
            err = esp_http_client_set_header(client, "Client-Id", uuid);
        }
        if (err == ESP_OK) {
            err = esp_http_client_set_header(client, "Activation-Version", "1");
        }
        if (err == ESP_OK) {
            err = esp_http_client_set_post_field(client, request_body, (int)strlen(request_body));
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "configure http client failed: %s", esp_err_to_name(err));
            goto attempt_cleanup;
        }

        log_ota_heap_state(attempt, max_attempts, "before http perform");
        err = esp_http_client_perform(client);
        if (err != ESP_OK) {
            log_ota_heap_state(attempt, max_attempts, "after http perform failed");
            if (err == ESP_ERR_HTTP_CONNECT) {
                ESP_LOGW(TAG, "HTTP connect failed after DNS resolved, check TLS/internal heap: %s", esp_err_to_name(err));
                retry_attempt = true;
                goto attempt_cleanup;
            }

            ESP_LOGE(TAG, "HTTP POST request failed: %s", esp_err_to_name(err));
            goto attempt_cleanup;
        }
        log_ota_heap_state(attempt, max_attempts, "after http perform ok");

        int status_code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "HTTP status=%d, response_len=%u", status_code, (unsigned int)response.len);
        if (status_code != 200) {
            ESP_LOGE(TAG, "OTA business error, status=%d", status_code);
            err = ESP_FAIL;
            goto attempt_cleanup;
        }

        err = parse_ota_response(response.data);

attempt_cleanup:
        if (client != NULL) {
            esp_http_client_cleanup(client);
            client = NULL;
        }
        response_buffer_free(&response);
        log_ota_heap_state(attempt, max_attempts, "after cleanup");
        if (retry_attempt) {
            delay_between_dns_retries(attempt, max_attempts);
            continue;
        }
        break;
    }

cleanup_common:
    heap_caps_free(user_agent);
    cJSON_free(request_body);
    return err;
}
