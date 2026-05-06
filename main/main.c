#include <stdio.h>

#include "esp_log.h"
#include "xiaozhi_button.h"

static const char *TAG = "app_main";

static void adc_button_cb(void *button_handle, void *usr_data);
static esp_err_t register_adc_button_events(int button_index);

void app_main(void)
{
	ESP_ERROR_CHECK(xiaozhi_button_init());
	ESP_ERROR_CHECK(register_adc_button_events(2));
	ESP_ERROR_CHECK(register_adc_button_events(3));
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
}
