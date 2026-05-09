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

    app_display_show_ble_qrcode(payload);
}

void app_ble_prov_show_status(const char *title, const char *message)
{
    app_display_show_message(title, message);
}
