#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XIAOZHI_UUID_STR_LEN 37
#define XIAOZHI_MAC_STR_LEN 18
#define XIAOZHI_IPV4_STR_LEN 16
#define XIAOZHI_ELF_SHA256_MIN_STR_LEN 65

esp_err_t xiaozhi_device_format_mac_str(const unsigned char mac_bytes[6], char *mac, size_t mac_size);
esp_err_t xiaozhi_device_get_or_create_uuid(char *uuid, size_t uuid_size);
esp_err_t xiaozhi_device_get_mac_str(char *mac, size_t mac_size);
esp_err_t xiaozhi_device_get_ip_str(char *ip, size_t ip_size);
esp_err_t xiaozhi_device_get_wifi_info(char *ssid, size_t ssid_size, int *rssi, int *channel);
esp_err_t xiaozhi_device_get_elf_sha256(char *sha256, size_t sha256_size);
const char *xiaozhi_device_get_board_name(void);
const char *xiaozhi_device_get_board_type(void);
const char *xiaozhi_device_get_app_version(void);

#ifdef __cplusplus
}
#endif
