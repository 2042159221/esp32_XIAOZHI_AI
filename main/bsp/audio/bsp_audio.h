#pragma once

#include "driver/i2c_master.h"
#include "esp_codec_dev.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BSP_AUDIO_SAMPLE_RATE 16000
#define BSP_AUDIO_BITS_PER_SAMPLE 16
#define BSP_AUDIO_CHANNELS 1

esp_err_t bsp_audio_init(void);
esp_codec_dev_handle_t bsp_audio_get_codec(void);
i2c_master_bus_handle_t bsp_audio_get_i2c_bus(void);
esp_err_t bsp_audio_init_i2c_bus(void);
esp_err_t bsp_audio_open(void);
esp_err_t bsp_audio_open_with_sample_rate(int sample_rate);
int bsp_audio_get_current_sample_rate(void);
esp_err_t bsp_audio_set_volume(int volume);

#ifdef __cplusplus
}
#endif
