#include <stdio.h>

#include "app_provisioning_manager.h"
#include "esp_log.h"
#include "xiaozhi_button.h"

static const char *TAG = "app_main";

static void adc_button_cb(void *button_handle, void *usr_data);
static esp_err_t register_adc_button_events(int button_index);
static void start_business(void *user_ctx);
static void provisioning_state_cb(app_provisioning_state_t state, void *user_ctx);

void app_main(void)
{
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

	ESP_ERROR_CHECK(xiaozhi_button_init());
	ESP_ERROR_CHECK(register_adc_button_events(2));
	ESP_ERROR_CHECK(register_adc_button_events(3));
}

static void provisioning_state_cb(app_provisioning_state_t state, void *user_ctx)
{
	(void)user_ctx;
	ESP_LOGI(TAG, "provisioning state changed: %d", state);
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
