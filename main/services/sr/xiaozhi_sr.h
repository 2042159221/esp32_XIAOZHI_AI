#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_vad.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*xiaozhi_sr_vad_state_cb_t)(vad_state_t state, void *user_ctx);
typedef void (*xiaozhi_sr_wake_cb_t)(void *user_ctx);

typedef struct {
    xiaozhi_sr_vad_state_cb_t vad_state_cb;
    xiaozhi_sr_wake_cb_t wake_cb;
    void (*pcm_output_cb)(const uint8_t *data, size_t len, void *user_ctx);
    void *user_ctx;
} xiaozhi_sr_callbacks_t;

esp_err_t xiaozhi_sr_init(const xiaozhi_sr_callbacks_t *callbacks);
esp_err_t xiaozhi_sr_stop(void);
bool xiaozhi_sr_get_wake_flag(void);
void xiaozhi_sr_clear_wake_flag(void);

#ifdef __cplusplus
}
#endif
