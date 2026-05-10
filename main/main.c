#include <stdio.h>

#include "app_display.h"
#include "app_provisioning_manager.h"
#include "app_status_led.h"
#include "esp_check.h"
#include "esp_log.h"
#include "xiaozhi_button.h"

static const char *TAG = "app_main";

static bool s_buttons_registered;

static void adc_button_cb(void *button_handle, void *usr_data);
static esp_err_t init_buttons_once(void);
static esp_err_t register_adc_button_events(int button_index);
static void start_business(void *user_ctx);
static void provisioning_state_cb(app_provisioning_state_t state, void *user_ctx);

void app_main(void)
{
	esp_err_t button_err = init_buttons_once();
	if (button_err != ESP_OK) {
		ESP_LOGE(TAG, "button init failed: %s", esp_err_to_name(button_err));
	}

	const app_provisioning_manager_config_t provisioning_config = {
		.business_start_cb = start_business,
		.state_cb = provisioning_state_cb,
		.user_ctx = NULL,
	};

	esp_err_t err = app_provisioning_manager_start(&provisioning_config);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "start provisioning manager failed: %s", esp_err_to_name(err));
	}
}

static void start_business(void *user_ctx)
{
	(void)user_ctx;
	esp_err_t display_err = app_display_init();
	if (display_err != ESP_OK) {
		ESP_LOGE(TAG, "display init failed after Wi-Fi connected: %s", esp_err_to_name(display_err));
	}
	app_display_show_message("Setup Complete", "Wi-Fi connected. XiaoZhi is ready");
	ESP_ERROR_CHECK(app_status_led_start());

	ESP_ERROR_CHECK(init_buttons_once());
}

static void provisioning_state_cb(app_provisioning_state_t state, void *user_ctx)
{
	(void)user_ctx;
	ESP_LOGI(TAG, "provisioning state changed: %d", state);
	if (!app_display_is_initialized()) {
		return;
	}

	switch (state) {
	case APP_PROVISIONING_STATE_UNPROVISIONED:
		app_display_show_message("XiaoZhi Wi-Fi Setup", "Starting scan-code provisioning");
		break;
	case APP_PROVISIONING_STATE_PROVISIONING:
		/* The QR payload callback owns the LCD in this state. */
		break;
	case APP_PROVISIONING_STATE_CRED_RECEIVED:
		app_display_show_message("Credentials Received", "Connecting to your Wi-Fi network");
		break;
	case APP_PROVISIONING_STATE_CONNECTING:
		app_display_show_message("Connecting", "Joining Wi-Fi. Keep the device powered on");
		break;
	case APP_PROVISIONING_STATE_CONNECTED:
		app_display_show_message("Wi-Fi Connected", "Saving setup and starting XiaoZhi");
		break;
	case APP_PROVISIONING_STATE_FAILED:
		app_display_show_message("Setup Failed", "Check Wi-Fi password. Scan code will return shortly");
		break;
	case APP_PROVISIONING_STATE_BUSINESS_STARTED:
		app_display_show_message("XiaoZhi Ready", "Network is online. You can use the device now");
		break;
	default:
		break;
	}
}

static esp_err_t init_buttons_once(void)
{
	if (s_buttons_registered) {
		return ESP_OK;
	}

	ESP_RETURN_ON_ERROR(xiaozhi_button_init(), TAG, "init ADC buttons failed");
	ESP_RETURN_ON_ERROR(register_adc_button_events(2), TAG, "register SW2 button events failed");
	ESP_RETURN_ON_ERROR(register_adc_button_events(3), TAG, "register SW3 button events failed");

	s_buttons_registered = true;
	ESP_LOGI(TAG, "ADC button events registered");
	return ESP_OK;
}

static esp_err_t register_adc_button_events(int button_index)
{
	ESP_ERROR_CHECK(xiaozhi_button_register_cb(button_index, BUTTON_PRESS_DOWN, adc_button_cb, (void *)button_index));
	ESP_ERROR_CHECK(xiaozhi_button_register_cb(button_index, BUTTON_PRESS_UP, adc_button_cb, (void *)button_index));
	ESP_ERROR_CHECK(xiaozhi_button_register_cb(button_index, BUTTON_SINGLE_CLICK, adc_button_cb, (void *)button_index));
	ESP_ERROR_CHECK(xiaozhi_button_register_cb(button_index, BUTTON_DOUBLE_CLICK, adc_button_cb, (void *)button_index));
	ESP_ERROR_CHECK(xiaozhi_button_register_cb(button_index, BUTTON_LONG_PRESS_START, adc_button_cb, (void *)button_index));
	ESP_ERROR_CHECK(xiaozhi_button_register_cb(button_index, BUTTON_LONG_PRESS_HOLD, adc_button_cb, (void *)button_index));
	ESP_ERROR_CHECK(xiaozhi_button_register_cb(button_index, BUTTON_LONG_PRESS_UP, adc_button_cb, (void *)button_index));

	return ESP_OK;
}

static void adc_button_cb(void *button_handle, void *usr_data)
{
	int button_index = (int)usr_data;
	button_event_t event = iot_button_get_event(button_handle);

	ESP_LOGW(TAG, "adc button %d %s", button_index, iot_button_get_event_str(event));
	if (button_index == 2 && event == BUTTON_SINGLE_CLICK) {
		ESP_LOGW(TAG, "");
		ESP_LOGW(TAG, "========== SW2 RESET PROVISIONING ==========");
		ESP_LOGW(TAG, "SW2 single click detected");
		ESP_LOGW(TAG, "clearing saved WiFi/provisioning data now");
		ESP_LOGW(TAG, "device will restart and prefer BLE provisioning");
		ESP_LOGW(TAG, "===========================================");
		ESP_ERROR_CHECK(app_provisioning_manager_reset_and_restart());
	}
}
