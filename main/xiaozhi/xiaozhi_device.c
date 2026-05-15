#include "xiaozhi_device.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "xiaozhi_device";

#define XIAOZHI_NVS_NAMESPACE "xiaozhi"
#define XIAOZHI_NVS_UUID_KEY "uuid"
#define XIAOZHI_UUID_VALUE_LEN 36

#ifndef CONFIG_XIAOZHI_BOARD_NAME
#define CONFIG_XIAOZHI_BOARD_NAME "atguigu-ai-xiaozhi-doorbell"
#endif

#ifndef CONFIG_XIAOZHI_BOARD_TYPE
#define CONFIG_XIAOZHI_BOARD_TYPE "esp32s3"
#endif

static bool is_uuid_hex_char(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static bool is_valid_uuid_v4(const char *uuid)
{
    if (uuid == NULL || strlen(uuid) != XIAOZHI_UUID_VALUE_LEN) {
        return false;
    }

    for (size_t i = 0; i < XIAOZHI_UUID_VALUE_LEN; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (uuid[i] != '-') {
                return false;
            }
            continue;
        }

        if (!is_uuid_hex_char(uuid[i])) {
            return false;
        }
    }

    if (uuid[14] != '4') {
        return false;
    }

    char variant = uuid[19];
    return variant == '8' || variant == '9' || variant == 'a' || variant == 'b' || variant == 'A' || variant == 'B';
}

static void generate_uuid_v4(char *uuid, size_t uuid_size)
{
    uint8_t bytes[16];

    for (size_t i = 0; i < sizeof(bytes); i += 4) {
        uint32_t random = esp_random();
        size_t copy_len = sizeof(bytes) - i;
        if (copy_len > sizeof(random)) {
            copy_len = sizeof(random);
        }
        memcpy(&bytes[i], &random, copy_len);
    }

    bytes[6] = (bytes[6] & 0x0F) | 0x40;
    bytes[8] = (bytes[8] & 0x3F) | 0x80;

    snprintf(uuid,
             uuid_size,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             bytes[0],
             bytes[1],
             bytes[2],
             bytes[3],
             bytes[4],
             bytes[5],
             bytes[6],
             bytes[7],
             bytes[8],
             bytes[9],
             bytes[10],
             bytes[11],
             bytes[12],
             bytes[13],
             bytes[14],
             bytes[15]);
}

esp_err_t xiaozhi_device_format_mac_str(const unsigned char mac_bytes[6], char *mac, size_t mac_size)
{
    ESP_RETURN_ON_FALSE(mac_bytes != NULL, ESP_ERR_INVALID_ARG, TAG, "mac bytes is null");
    ESP_RETURN_ON_FALSE(mac != NULL, ESP_ERR_INVALID_ARG, TAG, "mac output is null");
    ESP_RETURN_ON_FALSE(mac_size >= XIAOZHI_MAC_STR_LEN, ESP_ERR_INVALID_ARG, TAG, "mac buffer too small");

    snprintf(mac,
             mac_size,
             "%02x:%02x:%02x:%02x:%02x:%02x",
             mac_bytes[0],
             mac_bytes[1],
             mac_bytes[2],
             mac_bytes[3],
             mac_bytes[4],
             mac_bytes[5]);
    return ESP_OK;
}

esp_err_t xiaozhi_device_get_or_create_uuid(char *uuid, size_t uuid_size)
{
    ESP_RETURN_ON_FALSE(uuid != NULL, ESP_ERR_INVALID_ARG, TAG, "uuid output is null");
    ESP_RETURN_ON_FALSE(uuid_size >= XIAOZHI_UUID_STR_LEN, ESP_ERR_INVALID_ARG, TAG, "uuid buffer too small");

    uuid[0] = '\0';

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(XIAOZHI_NVS_NAMESPACE, NVS_READWRITE, &handle);
    ESP_RETURN_ON_ERROR(err, TAG, "open xiaozhi nvs failed");

    size_t required_size = uuid_size;
    err = nvs_get_str(handle, XIAOZHI_NVS_UUID_KEY, uuid, &required_size);
    if (err == ESP_OK && is_valid_uuid_v4(uuid)) {
        nvs_close(handle);
        return ESP_OK;
    }

    if (err != ESP_ERR_NVS_NOT_FOUND && err != ESP_OK) {
        ESP_LOGW(TAG, "read stored uuid failed or invalid size: %s", esp_err_to_name(err));
    } else if (uuid[0] != '\0') {
        ESP_LOGW(TAG, "stored uuid is invalid, regenerating");
    }

    generate_uuid_v4(uuid, uuid_size);
    err = nvs_set_str(handle, XIAOZHI_NVS_UUID_KEY, uuid);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    ESP_RETURN_ON_ERROR(err, TAG, "save generated uuid failed");
    ESP_LOGI(TAG, "generated persistent UUID: %s", uuid);
    return ESP_OK;
}

esp_err_t xiaozhi_device_get_mac_str(char *mac, size_t mac_size)
{
    ESP_RETURN_ON_FALSE(mac != NULL, ESP_ERR_INVALID_ARG, TAG, "mac output is null");
    ESP_RETURN_ON_FALSE(mac_size >= XIAOZHI_MAC_STR_LEN, ESP_ERR_INVALID_ARG, TAG, "mac buffer too small");

    uint8_t mac_bytes[6] = {0};
    esp_err_t err = esp_wifi_get_mac(WIFI_IF_STA, mac_bytes);
    if (err == ESP_ERR_WIFI_NOT_INIT) {
        err = esp_read_mac(mac_bytes, ESP_MAC_WIFI_STA);
    }
    ESP_RETURN_ON_ERROR(err, TAG, "get wifi sta mac failed");
    return xiaozhi_device_format_mac_str(mac_bytes, mac, mac_size);
}

esp_err_t xiaozhi_device_get_ip_str(char *ip, size_t ip_size)
{
    ESP_RETURN_ON_FALSE(ip != NULL, ESP_ERR_INVALID_ARG, TAG, "ip output is null");
    ESP_RETURN_ON_FALSE(ip_size >= XIAOZHI_IPV4_STR_LEN, ESP_ERR_INVALID_ARG, TAG, "ip buffer too small");

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    ESP_RETURN_ON_FALSE(netif != NULL, ESP_ERR_NOT_FOUND, TAG, "wifi sta netif not found");

    esp_netif_ip_info_t ip_info = {0};
    ESP_RETURN_ON_ERROR(esp_netif_get_ip_info(netif, &ip_info), TAG, "get ip info failed");

    snprintf(ip, ip_size, IPSTR, IP2STR(&ip_info.ip));
    return ESP_OK;
}

esp_err_t xiaozhi_device_get_wifi_info(char *ssid, size_t ssid_size, int *rssi, int *channel)
{
    ESP_RETURN_ON_FALSE(ssid != NULL, ESP_ERR_INVALID_ARG, TAG, "ssid output is null");
    ESP_RETURN_ON_FALSE(ssid_size > 0, ESP_ERR_INVALID_ARG, TAG, "ssid buffer is empty");
    ESP_RETURN_ON_FALSE(rssi != NULL, ESP_ERR_INVALID_ARG, TAG, "rssi output is null");
    ESP_RETURN_ON_FALSE(channel != NULL, ESP_ERR_INVALID_ARG, TAG, "channel output is null");

    wifi_ap_record_t ap_info = {0};
    ESP_RETURN_ON_ERROR(esp_wifi_sta_get_ap_info(&ap_info), TAG, "get wifi ap info failed");

    snprintf(ssid, ssid_size, "%s", (const char *)ap_info.ssid);
    *rssi = ap_info.rssi;
    *channel = ap_info.primary;
    return ESP_OK;
}

esp_err_t xiaozhi_device_get_elf_sha256(char *sha256, size_t sha256_size)
{
    ESP_RETURN_ON_FALSE(sha256 != NULL, ESP_ERR_INVALID_ARG, TAG, "sha256 output is null");
    ESP_RETURN_ON_FALSE(sha256_size > 0, ESP_ERR_INVALID_ARG, TAG, "sha256 buffer is empty");

    const char *elf_sha256 = esp_app_get_elf_sha256_str();
    ESP_RETURN_ON_FALSE(elf_sha256 != NULL, ESP_FAIL, TAG, "elf sha256 is null");

    snprintf(sha256, sha256_size, "%s", elf_sha256);
    return ESP_OK;
}

const char *xiaozhi_device_get_board_name(void)
{
    return CONFIG_XIAOZHI_BOARD_NAME;
}

const char *xiaozhi_device_get_board_type(void)
{
    return CONFIG_XIAOZHI_BOARD_TYPE;
}

const char *xiaozhi_device_get_app_version(void)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();
    return app_desc != NULL ? app_desc->version : "0.0.0";
}
