#include "app_controller.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_platform_init.h"

static const char *TAG = "app_main";

void app_main(void)
{
    esp_err_t err = esp_platform_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init platform failed: %s", esp_err_to_name(err));
        return;
    }

    err = app_controller_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start app controller failed: %s", esp_err_to_name(err));
    }
}
