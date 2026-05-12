#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "esp_prov_strategy.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t provisioning_qr_payload_build(const esp_prov_strategy_t *strategy,
                                        const char *service_name,
                                        char *payload,
                                        size_t payload_size);
esp_err_t provisioning_qr_payload_alloc(const esp_prov_strategy_t *strategy,
                                        const char *service_name,
                                        char **out_payload);

#ifdef __cplusplus
}
#endif
