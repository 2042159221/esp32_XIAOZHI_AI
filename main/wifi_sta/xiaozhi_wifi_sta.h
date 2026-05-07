#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_wifi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XIAOZHI_WIFI_STA_MAX_SSID_LEN 32
#define XIAOZHI_WIFI_STA_MAX_PASSWORD_LEN 64

typedef struct {
    char ssid[XIAOZHI_WIFI_STA_MAX_SSID_LEN + 1];
    char password[XIAOZHI_WIFI_STA_MAX_PASSWORD_LEN + 1];
    uint8_t max_retry;
    wifi_auth_mode_t authmode;
} xiaozhi_wifi_sta_config_t;

esp_err_t xiaozhi_wifi_sta_init(void);
esp_err_t xiaozhi_wifi_sta_connect_saved(void);
esp_err_t xiaozhi_wifi_sta_connect_default(void);
esp_err_t xiaozhi_wifi_sta_connect(const xiaozhi_wifi_sta_config_t *config);
esp_err_t xiaozhi_wifi_sta_disconnect(void);
bool xiaozhi_wifi_sta_is_connected(void);

#ifdef __cplusplus
}
#endif