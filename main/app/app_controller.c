#include "app_controller.h"

#include "app_input_controller.h"
#include "esp_check.h"
#include "esp_log.h"
#include "provisioning_screen.h"
#include "provisioning_service.h"
#include "status_led_service.h"
#include "xiaozhi_stage1.h"
#include "xiaozhi_ui.h"

static const char *TAG = "app_controller";

#define UI_TEXT_XIAOZHI_TITLE "\xE5\xB0\x9A\xE7\xA1\x85\xE8\xB0\xB7" "AI" "\xE5\xB0\x8F\xE6\x99\xBA"
#define UI_TEXT_START_PROV "\xE6\xAD\xA3\xE5\x9C\xA8\xE5\x90\xAF\xE5\x8A\xA8\xE6\x89\xAB\xE7\xA0\x81\xE9\x85\x8D\xE7\xBD\x91"
#define UI_TEXT_SCAN_PROV "\xE8\xAF\xB7\xE6\x89\xAB\xE7\xA0\x81\xE9\x85\x8D\xE7\xBD\x91!"
#define UI_TEXT_MAKE_QR "\xE6\xAD\xA3\xE5\x9C\xA8\xE7\x94\x9F\xE6\x88\x90\xE9\x85\x8D\xE7\xBD\x91\xE4\xBA\x8C\xE7\xBB\xB4\xE7\xA0\x81"
#define UI_TEXT_WIFI_GOT "\xE6\x94\xB6\xE5\x88\xB0WiFi\xE4\xBF\xA1\xE6\x81\xAF"
#define UI_TEXT_WIFI_JOINING "\xE6\xAD\xA3\xE5\x9C\xA8\xE8\xBF\x9E\xE6\x8E\xA5\xE6\x97\xA0\xE7\xBA\xBF\xE7\xBD\x91\xE7\xBB\x9C"
#define UI_TEXT_WIFI_CONNECTING "\xE6\xAD\xA3\xE5\x9C\xA8\xE8\xBF\x9E\xE6\x8E\xA5WiFi"
#define UI_TEXT_KEEP_POWER "\xE8\xAF\xB7\xE4\xBF\x9D\xE6\x8C\x81\xE8\xAE\xBE\xE5\xA4\x87\xE9\x80\x9A\xE7\x94\xB5"
#define UI_TEXT_WIFI_CONNECTED "\x57\x69\x46\x69\xE5\xB7\xB2\xE8\xBF\x9E\xE6\x8E\xA5"
#define UI_TEXT_START_XIAOZHI "\xE6\xAD\xA3\xE5\x9C\xA8\xE5\x90\xAF\xE5\x8A\xA8\xE5\xB0\x8F\xE6\x99\xBA"
#define UI_TEXT_PROV_FAILED "\xE9\x85\x8D\xE7\xBD\x91\xE5\xA4\xB1\xE8\xB4\xA5"
#define UI_TEXT_CHECK_WIFI "\xE8\xAF\xB7\xE6\xA3\x80\xE6\x9F\xA5WiFi\xE5\xAF\x86\xE7\xA0\x81\xEF\xBC\x8C\xE7\xA8\x8D\xE5\x90\x8E\xE9\x87\x8D\xE6\x96\xB0\xE6\x89\xAB\xE7\xA0\x81"
#define UI_TEXT_CONNECTING_XIAOZHI "\xE6\xAD\xA3\xE5\x9C\xA8\xE8\xBF\x9E\xE6\x8E\xA5\xE5\xB0\x8F\xE6\x99\xBA"
#define UI_TEXT_NET_READY "\xE7\xBD\x91\xE7\xBB\x9C\xE5\xB7\xB2\xE5\xB0\xB1\xE7\xBB\xAA"

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

    esp_err_t display_err = xiaozhi_ui_init();
    if (display_err != ESP_OK) {
        ESP_LOGE(TAG, "display init failed after Wi-Fi connected: %s", esp_err_to_name(display_err));
    } else {
        xiaozhi_ui_show_ota_loading();
    }

    esp_err_t led_err = status_led_service_start();
    if (led_err != ESP_OK) {
        ESP_LOGE(TAG, "start status led failed: %s", esp_err_to_name(led_err));
    }

    esp_err_t xiaozhi_err = xiaozhi_stage1_start();
    if (xiaozhi_err != ESP_OK) {
        ESP_LOGE(TAG, "start xiaozhi stage1 failed: %s", esp_err_to_name(xiaozhi_err));
    }
}

static void provisioning_state_cb(provisioning_service_state_t state, void *user_ctx)
{
    (void)user_ctx;
    ESP_LOGI(TAG, "provisioning state changed: %d", state);

    switch (state) {
    case PROVISIONING_SERVICE_STATE_UNPROVISIONED:
        provisioning_screen_show_status(UI_TEXT_XIAOZHI_TITLE, UI_TEXT_START_PROV);
        break;
    case PROVISIONING_SERVICE_STATE_PROVISIONING:
        provisioning_screen_show_status(UI_TEXT_SCAN_PROV, UI_TEXT_MAKE_QR);
        break;
    case PROVISIONING_SERVICE_STATE_CRED_RECEIVED:
        provisioning_screen_show_status(UI_TEXT_WIFI_GOT, UI_TEXT_WIFI_JOINING);
        break;
    case PROVISIONING_SERVICE_STATE_CONNECTING:
        provisioning_screen_show_status(UI_TEXT_WIFI_CONNECTING, UI_TEXT_KEEP_POWER);
        break;
    case PROVISIONING_SERVICE_STATE_CONNECTED:
        provisioning_screen_show_status(UI_TEXT_WIFI_CONNECTED, UI_TEXT_START_XIAOZHI);
        break;
    case PROVISIONING_SERVICE_STATE_FAILED:
        provisioning_screen_show_status(UI_TEXT_PROV_FAILED, UI_TEXT_CHECK_WIFI);
        break;
    case PROVISIONING_SERVICE_STATE_BUSINESS_STARTED:
        provisioning_screen_show_status(UI_TEXT_CONNECTING_XIAOZHI, UI_TEXT_NET_READY);
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
