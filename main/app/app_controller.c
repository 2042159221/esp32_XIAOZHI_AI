#include "app_controller.h"

#include "app_input_controller.h"
#include "display_service.h"
#include "esp_check.h"
#include "esp_log.h"
#include "provisioning_screen.h"
#include "provisioning_service.h"
#include "status_led_service.h"

static const char *TAG = "app_controller";

static void business_start_cb(void *user_ctx);
static void provisioning_state_cb(provisioning_service_state_t state, void *user_ctx);
static void provisioning_qrcode_cb(const char *payload, void *user_ctx);
static esp_err_t reset_provisioning_cb(void *user_ctx);

esp_err_t app_controller_start(void)
{
    const app_input_controller_config_t input_config = {
        .reset_provisioning_cb = reset_provisioning_cb,
        .user_ctx = NULL,
    };
    ESP_RETURN_ON_ERROR(app_input_controller_init(&input_config), TAG, "init input controller failed");

    const provisioning_service_config_t provisioning_config = {
        .business_start_cb = business_start_cb,
        .state_cb = provisioning_state_cb,
        .qrcode_cb = provisioning_qrcode_cb,
        .user_ctx = NULL,
    };

    ESP_RETURN_ON_ERROR(provisioning_service_start(&provisioning_config), TAG, "start provisioning service failed");
    return ESP_OK;
}

static void business_start_cb(void *user_ctx)
{
    (void)user_ctx;

    esp_err_t display_err = display_service_init();
    if (display_err != ESP_OK) {
        ESP_LOGE(TAG, "display init failed after Wi-Fi connected: %s", esp_err_to_name(display_err));
    } else {
        display_service_show_message("Setup Complete", "Wi-Fi connected. XiaoZhi is ready");
    }

    esp_err_t led_err = status_led_service_start();
    if (led_err != ESP_OK) {
        ESP_LOGE(TAG, "start status led failed: %s", esp_err_to_name(led_err));
    }
}

static void provisioning_state_cb(provisioning_service_state_t state, void *user_ctx)
{
    (void)user_ctx;
    ESP_LOGI(TAG, "provisioning state changed: %d", state);

    switch (state) {
    case PROVISIONING_SERVICE_STATE_UNPROVISIONED:
        provisioning_screen_show_status("XiaoZhi Wi-Fi Setup", "Starting scan-code provisioning");
        break;
    case PROVISIONING_SERVICE_STATE_PROVISIONING:
        provisioning_screen_show_status("XiaoZhi Wi-Fi Setup", "Preparing provisioning QR code");
        break;
    case PROVISIONING_SERVICE_STATE_CRED_RECEIVED:
        provisioning_screen_show_status("Credentials Received", "Connecting to your Wi-Fi network");
        break;
    case PROVISIONING_SERVICE_STATE_CONNECTING:
        provisioning_screen_show_status("Connecting", "Joining Wi-Fi. Keep the device powered on");
        break;
    case PROVISIONING_SERVICE_STATE_CONNECTED:
        provisioning_screen_show_status("Wi-Fi Connected", "Saving setup and starting XiaoZhi");
        break;
    case PROVISIONING_SERVICE_STATE_FAILED:
        provisioning_screen_show_status("Setup Failed", "Check Wi-Fi password. Scan code will return shortly");
        break;
    case PROVISIONING_SERVICE_STATE_BUSINESS_STARTED:
        provisioning_screen_show_status("XiaoZhi Ready", "Network is online. You can use the device now");
        break;
    default:
        break;
    }
}

static void provisioning_qrcode_cb(const char *payload, void *user_ctx)
{
    (void)user_ctx;
    provisioning_screen_show_qrcode(payload);
}

static esp_err_t reset_provisioning_cb(void *user_ctx)
{
    (void)user_ctx;
    return provisioning_service_reset_and_restart();
}
