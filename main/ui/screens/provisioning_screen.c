#include "provisioning_screen.h"

#include "display_service.h"
#include "esp_log.h"

static const char *TAG = "prov_screen";

void provisioning_screen_show_qrcode(const char *payload)
{
    if (payload == NULL) {
        ESP_LOGW(TAG, "skip QR display because payload is null");
        return;
    }

    esp_err_t err = display_service_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "init display for provisioning QR failed: %s", esp_err_to_name(err));
        return;
    }

    display_service_show_qrcode(payload);
}

void provisioning_screen_show_status(const char *title, const char *message)
{
    esp_err_t err = display_service_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "init display for provisioning status failed: %s", esp_err_to_name(err));
        return;
    }

    display_service_show_message(title, message);
}
