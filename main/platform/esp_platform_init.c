#include "esp_platform_init.h"

#include <stdbool.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"

static const char *TAG = "platform_init";

static bool s_platform_initialized;

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase nvs failed");
        err = nvs_flash_init();
    }

    return err;
}

esp_err_t esp_platform_init(void)
{
    if (s_platform_initialized) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(init_nvs(), TAG, "init nvs failed");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "init esp netif failed");

    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(err, TAG, "create default event loop failed");
    }

    s_platform_initialized = true;
    ESP_LOGI(TAG, "ESP platform services initialized");
    return ESP_OK;
}
