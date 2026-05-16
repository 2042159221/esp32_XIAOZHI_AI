#include "app_controller.h"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_platform_init.h"

static const char *TAG = "app_main";

static void heap_failed_cb(size_t size, uint32_t caps, const char *function_name)
{
    ESP_LOGE(TAG,
             "heap alloc failed size=%u caps=0x%08x function=%s internal_free=%u internal_largest=%u spiram_free=%u spiram_largest=%u",
             (unsigned int)size,
             (unsigned int)caps,
             function_name != NULL ? function_name : "<unknown>",
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

void app_main(void)
{
    esp_err_t err = heap_caps_register_failed_alloc_callback(heap_failed_cb);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "register heap failed callback failed: %s", esp_err_to_name(err));
    }

    err = esp_platform_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init platform failed: %s", esp_err_to_name(err));
        return;
    }

    err = app_controller_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start app controller failed: %s", esp_err_to_name(err));
    }
}
