#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_wifi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_STA_SERVICE_MAX_SSID_LEN 32
#define WIFI_STA_SERVICE_MAX_PASSWORD_LEN 64

typedef struct {
    char ssid[WIFI_STA_SERVICE_MAX_SSID_LEN + 1];
    char password[WIFI_STA_SERVICE_MAX_PASSWORD_LEN + 1];
    uint8_t max_retry;
    wifi_auth_mode_t authmode;
} wifi_sta_service_config_t;

esp_err_t wifi_sta_service_init(void);
esp_err_t wifi_sta_service_connect_saved(void);
esp_err_t wifi_sta_service_connect_default(void);
esp_err_t wifi_sta_service_connect(const wifi_sta_service_config_t *config);
esp_err_t wifi_sta_service_wait_connected(uint32_t timeout_ms);
esp_err_t wifi_sta_service_disconnect(void);
bool wifi_sta_service_is_connected(void);

#ifdef __cplusplus
}
#endif
