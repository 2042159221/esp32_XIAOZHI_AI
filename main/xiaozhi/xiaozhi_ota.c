#include "xiaozhi_ota.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "xiaozhi_device.h"
#include "xiaozhi_handle.h"

static const char *TAG = "xiaozhi_ota";

#define XIAOZHI_HTTP_RESPONSE_MAX_LEN (32 * 1024)

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} xiaozhi_http_response_t;

static void response_buffer_free(xiaozhi_http_response_t *response)
{
    if (response == NULL) {
        return;
    }

    free(response->data);
    response->data = NULL;
    response->len = 0;
    response->cap = 0;
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

        char *next = (char *)realloc(response->data, next_cap);
        ESP_RETURN_ON_FALSE(next != NULL, ESP_ERR_NO_MEM, TAG, "realloc http response failed");
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

    char *user_agent = (char *)malloc((size_t)required + 1);
    if (user_agent == NULL) {
        return NULL;
    }

    int written = snprintf(user_agent, (size_t)required + 1, "%s/%s %s", board_name, app_version, board_type);
    if (written != required) {
        free(user_agent);
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

    xiaozhi_http_response_t response = {0};
    esp_http_client_config_t http_config = {
        .url = ota_url,
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_handler,
        .user_data = &response,
        .timeout_ms = timeout_ms,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (client == NULL) {
        free(user_agent);
        cJSON_free(request_body);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "request ota url=%s timeout=%dms", ota_url, timeout_ms);
    ESP_LOGI(TAG, "Client-Id(UUID)=%s", uuid);
    ESP_LOGI(TAG, "Device-Id(MAC)=%s", mac);

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
        goto cleanup;
    }

    err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP POST request failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    int status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP status=%d, response_len=%u", status_code, (unsigned int)response.len);
    if (status_code != 200) {
        ESP_LOGE(TAG, "OTA request rejected, status=%d", status_code);
        err = ESP_FAIL;
        goto cleanup;
    }

    err = parse_ota_response(response.data);

cleanup:
    esp_http_client_cleanup(client);
    free(user_agent);
    cJSON_free(request_body);
    response_buffer_free(&response);
    return err;
}
