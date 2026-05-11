#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CONFIG_XIAOZHI_OTA_URL
#define CONFIG_XIAOZHI_OTA_URL "https://api.tenclass.net/xiaozhi/ota/"
#endif

#ifndef CONFIG_XIAOZHI_HTTP_TIMEOUT_MS
#define CONFIG_XIAOZHI_HTTP_TIMEOUT_MS 10000
#endif

#define XIAOZHI_DEFAULT_OTA_URL CONFIG_XIAOZHI_OTA_URL

typedef struct {
    const char *ota_url;
    int timeout_ms;
} xiaozhi_ota_config_t;

esp_err_t xiaozhi_ota_request(const xiaozhi_ota_config_t *config);

#ifdef __cplusplus
}
#endif
