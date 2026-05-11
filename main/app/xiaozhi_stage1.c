#include "xiaozhi_stage1.h"

#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "xiaozhi_device.h"
#include "xiaozhi_handle.h"
#include "xiaozhi_ota.h"
#include "xiaozhi_ui.h"

static const char *TAG = "xiaozhi_stage1";

#define XIAOZHI_STAGE1_OTA_TASK_STACK 8192
#define XIAOZHI_STAGE1_OTA_TASK_PRIORITY 5
#define UI_TEXT_CONNECT_FAILED "\xE8\xBF\x9E\xE6\x8E\xA5\xE5\xA4\xB1\xE8\xB4\xA5"
#define UI_TEXT_INIT_FAILED "\xE8\xAE\xBE\xE5\xA4\x87\xE5\x88\x9D\xE5\xA7\x8B\xE5\x8C\x96\xE5\xA4\xB1\xE8\xB4\xA5"
#define UI_TEXT_CHECK_NETWORK "\xE8\xAF\xB7\xE6\xA3\x80\xE6\x9F\xA5\xE7\xBD\x91\xE7\xBB\x9C\xE6\x88\x96\xE6\x9C\x8D\xE5\x8A\xA1\xE5\x99\xA8"
#define UI_TEXT_TASK_CREATE_FAILED "\x4F\x54\x41\xE4\xBB\xBB\xE5\x8A\xA1\xE5\x88\x9B\xE5\xBB\xBA\xE5\xA4\xB1\xE8\xB4\xA5"

static TaskHandle_t s_ota_task_handle;

static void log_token_len(void)
{
    const char *token = xiaozhi_handle_get_websocket_token();
    ESP_LOGI(TAG, "websocket token length=%u", token != NULL ? (unsigned int)strlen(token) : 0U);
}

static void log_device_snapshot(void)
{
    char uuid[XIAOZHI_UUID_STR_LEN] = {0};
    char mac[XIAOZHI_MAC_STR_LEN] = {0};
    char ip[XIAOZHI_IPV4_STR_LEN] = {0};
    char ssid[33] = {0};
    int rssi = 0;
    int channel = 0;

    esp_err_t err = xiaozhi_device_get_or_create_uuid(uuid, sizeof(uuid));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "UUID=%s", uuid);
    } else {
        ESP_LOGW(TAG, "get UUID failed: %s", esp_err_to_name(err));
    }

    err = xiaozhi_device_get_mac_str(mac, sizeof(mac));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "MAC=%s", mac);
    } else {
        ESP_LOGW(TAG, "get MAC failed: %s", esp_err_to_name(err));
    }

    err = xiaozhi_device_get_ip_str(ip, sizeof(ip));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "IP=%s", ip);
    } else {
        ESP_LOGW(TAG, "get IP failed: %s", esp_err_to_name(err));
    }

    err = xiaozhi_device_get_wifi_info(ssid, sizeof(ssid), &rssi, &channel);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "WiFi ssid=%s rssi=%d channel=%d", ssid, rssi, channel);
    } else {
        ESP_LOGW(TAG, "get WiFi info failed: %s", esp_err_to_name(err));
    }
}

static void ota_task(void *arg)
{
    (void)arg;

    esp_err_t err = xiaozhi_handle_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init xiaozhi handle failed: %s", esp_err_to_name(err));
        xiaozhi_ui_show_error(UI_TEXT_CONNECT_FAILED, UI_TEXT_INIT_FAILED);
        goto cleanup;
    }

    xiaozhi_handle_clear_runtime();
    xiaozhi_ui_show_ota_loading();
    log_device_snapshot();

    const xiaozhi_ota_config_t ota_config = {
        .ota_url = XIAOZHI_DEFAULT_OTA_URL,
        .timeout_ms = CONFIG_XIAOZHI_HTTP_TIMEOUT_MS,
    };

    err = xiaozhi_ota_request(&ota_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "xiaozhi ota request failed: %s", esp_err_to_name(err));
        xiaozhi_ui_show_error(UI_TEXT_CONNECT_FAILED, UI_TEXT_CHECK_NETWORK);
        goto cleanup;
    }

    bool activated = xiaozhi_handle_is_activated();
    const char *activation_code = xiaozhi_handle_get_activation_code();
    ESP_LOGI(TAG, "activated=%s", activated ? "true" : "false");
    ESP_LOGI(TAG, "activation code=%s", activation_code != NULL ? activation_code : "<none>");
    ESP_LOGI(TAG, "websocket url present=%s", xiaozhi_handle_get_websocket_url() != NULL ? "yes" : "no");
    log_token_len();

    if (activated) {
        xiaozhi_ui_show_welcome();
    } else {
        xiaozhi_ui_show_activation_required(activation_code);
    }

cleanup:
    s_ota_task_handle = NULL;
    vTaskDelete(NULL);
}

esp_err_t xiaozhi_stage1_start(void)
{
    if (s_ota_task_handle != NULL) {
        ESP_LOGW(TAG, "xiaozhi ota task is already running");
        return ESP_OK;
    }

    BaseType_t created = xTaskCreate(ota_task,
                                     "xiaozhi_ota",
                                     XIAOZHI_STAGE1_OTA_TASK_STACK,
                                     NULL,
                                     XIAOZHI_STAGE1_OTA_TASK_PRIORITY,
                                     &s_ota_task_handle);
    if (created != pdPASS) {
        s_ota_task_handle = NULL;
        ESP_LOGE(TAG, "create xiaozhi ota task failed");
        xiaozhi_ui_show_error(UI_TEXT_CONNECT_FAILED, UI_TEXT_TASK_CREATE_FAILED);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "xiaozhi ota task started");
    return ESP_OK;
}
