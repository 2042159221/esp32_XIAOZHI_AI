#include "app_ble_prov.h"

#include "app_display.h"
#include "esp_log.h"

static const char *TAG = "app_ble_prov";

void app_ble_prov_show_qrcode(const char *payload)
{
    if (payload == NULL) {
        ESP_LOGW(TAG, "skip BLE QR display because payload is null");
        return;
    }

    esp_err_t err = app_display_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "init display for BLE QR failed: %s", esp_err_to_name(err));
        return;
    }

    app_display_show_ble_qrcode(payload);
}

void app_ble_prov_show_status(const char *title, const char *message)
{
    if (!app_display_is_initialized()) {
        return;
    }

    app_display_show_message(title, message);
}
