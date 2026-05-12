#include "provisioning_qr_payload.h"

#include <stdio.h>
#include <stdlib.h>

#include "esp_check.h"
#include "sdkconfig.h"

static const char *TAG = "prov_qr_payload";

#define PROVISIONING_QR_PAYLOAD_FORMAT "{\"ver\":\"%s\",\"name\":\"%s\",\"pop\":\"%s\",\"transport\":\"%s\",\"security\":\"1\",\"capabilities\":[\"%s\"]}"

#ifndef CONFIG_APP_PROV_POP
#define CONFIG_APP_PROV_POP "abcd1234"
#endif

static esp_err_t provisioning_qr_payload_format(const esp_prov_strategy_t *strategy,
                                                const char *service_name,
                                                char *payload,
                                                size_t payload_size,
                                                size_t *required_size)
{
    ESP_RETURN_ON_FALSE(strategy != NULL, ESP_ERR_INVALID_ARG, TAG, "strategy is null");
    ESP_RETURN_ON_FALSE(service_name != NULL, ESP_ERR_INVALID_ARG, TAG, "service name is null");
    ESP_RETURN_ON_FALSE(strategy->qr_version != NULL, ESP_ERR_INVALID_ARG, TAG, "strategy qr version is null");
    ESP_RETURN_ON_FALSE(strategy->transport != NULL, ESP_ERR_INVALID_ARG, TAG, "strategy transport is null");
    ESP_RETURN_ON_FALSE(strategy->capabilities != NULL, ESP_ERR_INVALID_ARG, TAG, "strategy capabilities is null");

    int written = snprintf(payload,
                           payload_size,
                           PROVISIONING_QR_PAYLOAD_FORMAT,
                           strategy->qr_version,
                           service_name,
                           CONFIG_APP_PROV_POP,
                           strategy->transport,
                           strategy->capabilities);
    ESP_RETURN_ON_FALSE(written >= 0, ESP_FAIL, TAG, "format provisioning QR payload failed");

    if (required_size != NULL) {
        *required_size = (size_t)written + 1;
    }

    if (payload != NULL) {
        ESP_RETURN_ON_FALSE((size_t)written < payload_size,
                            ESP_ERR_INVALID_SIZE,
                            TAG,
                            "provisioning QR payload is too long");
    }

    return ESP_OK;
}

esp_err_t provisioning_qr_payload_build(const esp_prov_strategy_t *strategy,
                                        const char *service_name,
                                        char *payload,
                                        size_t payload_size)
{
    ESP_RETURN_ON_FALSE(payload != NULL, ESP_ERR_INVALID_ARG, TAG, "payload buffer is null");
    ESP_RETURN_ON_FALSE(payload_size > 0, ESP_ERR_INVALID_ARG, TAG, "payload buffer is empty");

    return provisioning_qr_payload_format(strategy, service_name, payload, payload_size, NULL);
}

esp_err_t provisioning_qr_payload_alloc(const esp_prov_strategy_t *strategy,
                                        const char *service_name,
                                        char **out_payload)
{
    ESP_RETURN_ON_FALSE(out_payload != NULL, ESP_ERR_INVALID_ARG, TAG, "output payload is null");
    *out_payload = NULL;

    size_t required_size = 0;
    ESP_RETURN_ON_ERROR(provisioning_qr_payload_format(strategy, service_name, NULL, 0, &required_size),
                        TAG,
                        "calculate provisioning QR payload length failed");

    char *payload = (char *)malloc(required_size);
    ESP_RETURN_ON_FALSE(payload != NULL, ESP_ERR_NO_MEM, TAG, "alloc provisioning QR payload failed");

    esp_err_t err = provisioning_qr_payload_format(strategy, service_name, payload, required_size, NULL);
    if (err != ESP_OK) {
        free(payload);
        return err;
    }

    *out_payload = payload;
    return ESP_OK;
}
