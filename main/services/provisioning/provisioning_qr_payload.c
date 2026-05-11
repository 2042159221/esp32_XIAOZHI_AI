#include "provisioning_qr_payload.h"

#include <stdio.h>

#include "esp_check.h"
#include "sdkconfig.h"

static const char *TAG = "prov_qr_payload";

#ifndef CONFIG_APP_PROV_POP
#define CONFIG_APP_PROV_POP "abcd1234"
#endif

esp_err_t provisioning_qr_payload_build(const esp_prov_strategy_t *strategy,
                                        const char *service_name,
                                        char *payload,
                                        size_t payload_size)
{
    ESP_RETURN_ON_FALSE(strategy != NULL, ESP_ERR_INVALID_ARG, TAG, "strategy is null");
    ESP_RETURN_ON_FALSE(service_name != NULL, ESP_ERR_INVALID_ARG, TAG, "service name is null");
    ESP_RETURN_ON_FALSE(payload != NULL, ESP_ERR_INVALID_ARG, TAG, "payload buffer is null");
    ESP_RETURN_ON_FALSE(payload_size > 0, ESP_ERR_INVALID_ARG, TAG, "payload buffer is empty");

    int written = snprintf(payload,
                           payload_size,
                           "{\"ver\":\"%s\",\"name\":\"%s\",\"pop\":\"%s\",\"transport\":\"%s\",\"security\":\"1\",\"capabilities\":[\"%s\"]}",
                           strategy->qr_version,
                           service_name,
                           CONFIG_APP_PROV_POP,
                           strategy->transport,
                           strategy->capabilities);
    ESP_RETURN_ON_FALSE(written >= 0 && (size_t)written < payload_size,
                        ESP_ERR_INVALID_SIZE,
                        TAG,
                        "provisioning QR payload is too long");

    return ESP_OK;
}
