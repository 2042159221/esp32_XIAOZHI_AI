#include "audio_pcm_diag.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bsp_audio.h"
#include "board_pins.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "audio_diag";

#ifndef CONFIG_XIAOZHI_AUDIO_DIAG_TONE_MS
#define CONFIG_XIAOZHI_AUDIO_DIAG_TONE_MS 500
#endif

#ifndef CONFIG_XIAOZHI_AUDIO_DIAG_MIC_RMS_DURATION_MS
#define CONFIG_XIAOZHI_AUDIO_DIAG_MIC_RMS_DURATION_MS 5000
#endif

#ifndef CONFIG_XIAOZHI_AUDIO_DIAG_MIC_RMS_WINDOW_MS
#define CONFIG_XIAOZHI_AUDIO_DIAG_MIC_RMS_WINDOW_MS 500
#endif

#ifndef CONFIG_XIAOZHI_AUDIO_DIAG_LOOPBACK_MS
#define CONFIG_XIAOZHI_AUDIO_DIAG_LOOPBACK_MS 5000
#endif

#ifndef CONFIG_XIAOZHI_AUDIO_DIAG_VOLUME
#define CONFIG_XIAOZHI_AUDIO_DIAG_VOLUME 65
#endif

#ifndef CONFIG_XIAOZHI_AUDIO_DIAG_LOOPBACK_GAIN
#define CONFIG_XIAOZHI_AUDIO_DIAG_LOOPBACK_GAIN 1
#endif

#ifndef CONFIG_XIAOZHI_AUDIO_DIAG_LOOPBACK_LOG_MS
#define CONFIG_XIAOZHI_AUDIO_DIAG_LOOPBACK_LOG_MS 500
#endif

#ifndef CONFIG_XIAOZHI_AUDIO_DIAG_TONE_AMPLITUDE
#define CONFIG_XIAOZHI_AUDIO_DIAG_TONE_AMPLITUDE 12000
#endif

#define AUDIO_DIAG_FRAME_MS 20
#define AUDIO_DIAG_SAMPLES_PER_FRAME ((BSP_AUDIO_SAMPLE_RATE * AUDIO_DIAG_FRAME_MS) / 1000)
#define AUDIO_DIAG_BYTES_PER_FRAME (AUDIO_DIAG_SAMPLES_PER_FRAME * sizeof(int16_t))
#define AUDIO_DIAG_1KHZ_PERIOD_SAMPLES 16

static const int16_t s_1khz_lut[AUDIO_DIAG_1KHZ_PERIOD_SAMPLES] = {
    0,
    4592,
    8485,
    11086,
    CONFIG_XIAOZHI_AUDIO_DIAG_TONE_AMPLITUDE,
    11086,
    8485,
    4592,
    0,
    -4592,
    -8485,
    -11086,
    -CONFIG_XIAOZHI_AUDIO_DIAG_TONE_AMPLITUDE,
    -11086,
    -8485,
    -4592,
};

static uint32_t integer_sqrt_u64(uint64_t value)
{
    uint64_t op = value;
    uint64_t res = 0;
    uint64_t one = 1ULL << 62;

    while (one > op) {
        one >>= 2;
    }

    while (one != 0) {
        if (op >= res + one) {
            op -= res + one;
            res = (res >> 1) + one;
        } else {
            res >>= 1;
        }
        one >>= 2;
    }

    return (uint32_t)res;
}

static esp_err_t open_audio_for_diag(void)
{
    ESP_RETURN_ON_ERROR(bsp_audio_init_i2c_bus(), TAG, "init audio I2C bus failed");
    ESP_RETURN_ON_ERROR(bsp_audio_open(), TAG, "open audio codec failed");
    ESP_RETURN_ON_ERROR(bsp_audio_set_volume(CONFIG_XIAOZHI_AUDIO_DIAG_VOLUME), TAG, "set diag volume failed");
    return ESP_OK;
}

static void fill_1khz_frame(int16_t *frame, size_t sample_count, uint32_t sample_index_base)
{
    for (size_t i = 0; i < sample_count; ++i) {
        frame[i] = s_1khz_lut[(sample_index_base + i) % AUDIO_DIAG_1KHZ_PERIOD_SAMPLES];
    }
}

static int16_t clamp_i16(int32_t value)
{
    if (value > INT16_MAX) {
        return INT16_MAX;
    }
    if (value < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)value;
}

esp_err_t audio_diag_i2c_scan(void)
{
    ESP_RETURN_ON_ERROR(bsp_audio_init_i2c_bus(), TAG, "init audio I2C bus failed");
    i2c_master_bus_handle_t bus = bsp_audio_get_i2c_bus();
    ESP_RETURN_ON_FALSE(bus != NULL, ESP_ERR_INVALID_STATE, TAG, "audio I2C bus is not ready");

    ESP_LOGI(TAG, "start audio I2C scan on SDA=%d SCL=%d", BOARD_AUDIO_I2C_SDA, BOARD_AUDIO_I2C_SCL);

    size_t hit_count = 0;
    for (uint8_t address = 0x08; address <= 0x77; ++address) {
        esp_err_t err = i2c_master_probe(bus, address, 50);
        if (err == ESP_OK) {
            hit_count++;
            ESP_LOGI(TAG, "I2C device detected at 0x%02X", address);
        }
    }

    if (hit_count == 0) {
        ESP_LOGW(TAG, "audio I2C scan completed, no devices found");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "audio I2C scan completed, %u device(s) found", (unsigned int)hit_count);
    return ESP_OK;
}

esp_err_t audio_diag_play_1khz_tone(void)
{
    ESP_RETURN_ON_ERROR(open_audio_for_diag(), TAG, "prepare audio codec failed");
    ESP_RETURN_ON_FALSE(BSP_AUDIO_SAMPLE_RATE == 16000, ESP_ERR_NOT_SUPPORTED, TAG, "1 kHz tone diagnostics expect 16 kHz sample rate");
    ESP_RETURN_ON_FALSE(BSP_AUDIO_BITS_PER_SAMPLE == 16 && BSP_AUDIO_CHANNELS == 1, ESP_ERR_NOT_SUPPORTED, TAG, "audio diagnostics expect 16-bit mono PCM");

    int16_t frame[AUDIO_DIAG_SAMPLES_PER_FRAME];
    uint32_t total_frames = (CONFIG_XIAOZHI_AUDIO_DIAG_TONE_MS + AUDIO_DIAG_FRAME_MS - 1) / AUDIO_DIAG_FRAME_MS;
    if (total_frames == 0) {
        total_frames = 1;
    }

    int mute_ret = esp_codec_dev_set_out_mute(bsp_audio_get_codec(), false);
    if (mute_ret != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "unmute output before tone failed: %d", mute_ret);
    }

    ESP_LOGI(TAG, "play 1 kHz tone for %u ms, volume=%u", (unsigned int)CONFIG_XIAOZHI_AUDIO_DIAG_TONE_MS, (unsigned int)CONFIG_XIAOZHI_AUDIO_DIAG_VOLUME);
    for (uint32_t frame_index = 0; frame_index < total_frames; ++frame_index) {
        fill_1khz_frame(frame, AUDIO_DIAG_SAMPLES_PER_FRAME, frame_index * AUDIO_DIAG_SAMPLES_PER_FRAME);
        int ret = esp_codec_dev_write(bsp_audio_get_codec(), frame, AUDIO_DIAG_BYTES_PER_FRAME);
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "tone playback failed on frame %u: %d", (unsigned int)frame_index, ret);
            return ESP_FAIL;
        }
        vTaskDelay(pdMS_TO_TICKS(AUDIO_DIAG_FRAME_MS));
    }

    memset(frame, 0, sizeof(frame));
    (void)esp_codec_dev_write(bsp_audio_get_codec(), frame, AUDIO_DIAG_BYTES_PER_FRAME);
    mute_ret = esp_codec_dev_set_out_mute(bsp_audio_get_codec(), true);
    if (mute_ret != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "mute output after tone failed: %d", mute_ret);
    }
    ESP_LOGI(TAG, "1 kHz tone playback finished, output muted");
    return ESP_OK;
}

esp_err_t audio_diag_print_mic_rms(void)
{
    ESP_RETURN_ON_ERROR(open_audio_for_diag(), TAG, "prepare audio codec failed");
    ESP_RETURN_ON_FALSE(BSP_AUDIO_BITS_PER_SAMPLE == 16 && BSP_AUDIO_CHANNELS == 1, ESP_ERR_NOT_SUPPORTED, TAG, "audio diagnostics expect 16-bit mono PCM");

    int16_t frame[AUDIO_DIAG_SAMPLES_PER_FRAME];
    const uint32_t total_duration_ms = CONFIG_XIAOZHI_AUDIO_DIAG_MIC_RMS_DURATION_MS;
    const uint32_t window_ms = CONFIG_XIAOZHI_AUDIO_DIAG_MIC_RMS_WINDOW_MS;
    uint32_t elapsed_ms = 0;
    uint32_t window_elapsed_ms = 0;
    uint64_t window_sum_squares = 0;
    int16_t window_min = INT16_MAX;
    int16_t window_max = INT16_MIN;
    size_t window_samples = 0;

    ESP_LOGI(TAG, "print mic RMS for %u ms, window=%u ms", (unsigned int)total_duration_ms, (unsigned int)window_ms);
    while (elapsed_ms < total_duration_ms) {
        int ret = esp_codec_dev_read(bsp_audio_get_codec(), frame, AUDIO_DIAG_BYTES_PER_FRAME);
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "mic capture failed: %d", ret);
            vTaskDelay(pdMS_TO_TICKS(AUDIO_DIAG_FRAME_MS));
            continue;
        }

        for (size_t i = 0; i < AUDIO_DIAG_SAMPLES_PER_FRAME; ++i) {
            int16_t sample = frame[i];
            if (sample < window_min) {
                window_min = sample;
            }
            if (sample > window_max) {
                window_max = sample;
            }

            int32_t signed_sample = sample;
            window_sum_squares += (uint64_t)(signed_sample * signed_sample);
        }

        window_samples += AUDIO_DIAG_SAMPLES_PER_FRAME;
        window_elapsed_ms += AUDIO_DIAG_FRAME_MS;
        elapsed_ms += AUDIO_DIAG_FRAME_MS;

        if (window_elapsed_ms >= window_ms) {
            uint32_t rms = integer_sqrt_u64(window_sum_squares / window_samples);
            ESP_LOGI(TAG, "mic window=%u ms samples=%u min=%d max=%d rms=%u", (unsigned int)window_elapsed_ms, (unsigned int)window_samples, window_min, window_max, (unsigned int)rms);

            window_elapsed_ms = 0;
            window_sum_squares = 0;
            window_min = INT16_MAX;
            window_max = INT16_MIN;
            window_samples = 0;
        }
    }

    if (window_samples > 0) {
        uint32_t rms = integer_sqrt_u64(window_sum_squares / window_samples);
        ESP_LOGI(TAG, "mic final window=%u ms samples=%u min=%d max=%d rms=%u", (unsigned int)window_elapsed_ms, (unsigned int)window_samples, window_min, window_max, (unsigned int)rms);
    }

    ESP_LOGI(TAG, "mic RMS logging finished");
    return ESP_OK;
}

esp_err_t audio_diag_loopback(void)
{
    ESP_RETURN_ON_ERROR(open_audio_for_diag(), TAG, "prepare audio codec failed");
    ESP_RETURN_ON_FALSE(BSP_AUDIO_BITS_PER_SAMPLE == 16 && BSP_AUDIO_CHANNELS == 1, ESP_ERR_NOT_SUPPORTED, TAG, "audio diagnostics expect 16-bit mono PCM");

    int16_t frame[AUDIO_DIAG_SAMPLES_PER_FRAME];
    uint32_t total_frames = (CONFIG_XIAOZHI_AUDIO_DIAG_LOOPBACK_MS + AUDIO_DIAG_FRAME_MS - 1) / AUDIO_DIAG_FRAME_MS;
    if (total_frames == 0) {
        total_frames = 1;
    }

    uint32_t log_window_frames = (CONFIG_XIAOZHI_AUDIO_DIAG_LOOPBACK_LOG_MS + AUDIO_DIAG_FRAME_MS - 1) / AUDIO_DIAG_FRAME_MS;
    if (log_window_frames == 0) {
        log_window_frames = 1;
    }

    uint64_t in_sum_squares = 0;
    uint64_t out_sum_squares = 0;
    int16_t in_min = INT16_MAX;
    int16_t in_max = INT16_MIN;
    int16_t out_min = INT16_MAX;
    int16_t out_max = INT16_MIN;
    size_t log_samples = 0;
    uint32_t clipped_samples = 0;
    uint32_t log_elapsed_ms = 0;

    ESP_LOGW(TAG,
             "start local mic-to-speaker loopback for %u ms, volume=%u, digital_gain=%u",
             (unsigned int)CONFIG_XIAOZHI_AUDIO_DIAG_LOOPBACK_MS,
             (unsigned int)CONFIG_XIAOZHI_AUDIO_DIAG_VOLUME,
             (unsigned int)CONFIG_XIAOZHI_AUDIO_DIAG_LOOPBACK_GAIN);
    int mute_ret = esp_codec_dev_set_out_mute(bsp_audio_get_codec(), false);
    if (mute_ret != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "unmute output before loopback failed: %d", mute_ret);
    }

    for (uint32_t frame_index = 0; frame_index < total_frames; ++frame_index) {
        int ret = esp_codec_dev_read(bsp_audio_get_codec(), frame, AUDIO_DIAG_BYTES_PER_FRAME);
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "loopback capture failed on frame %u: %d", (unsigned int)frame_index, ret);
            return ESP_FAIL;
        }

        for (size_t i = 0; i < AUDIO_DIAG_SAMPLES_PER_FRAME; ++i) {
            int16_t in_sample = frame[i];
            if (in_sample < in_min) {
                in_min = in_sample;
            }
            if (in_sample > in_max) {
                in_max = in_sample;
            }
            int32_t signed_in_sample = in_sample;
            in_sum_squares += (uint64_t)(signed_in_sample * signed_in_sample);

            int32_t amplified = signed_in_sample * CONFIG_XIAOZHI_AUDIO_DIAG_LOOPBACK_GAIN;
            int16_t out_sample = clamp_i16(amplified);
            if (out_sample != amplified) {
                clipped_samples++;
            }
            frame[i] = out_sample;

            if (out_sample < out_min) {
                out_min = out_sample;
            }
            if (out_sample > out_max) {
                out_max = out_sample;
            }
            int32_t signed_out_sample = out_sample;
            out_sum_squares += (uint64_t)(signed_out_sample * signed_out_sample);
        }
        log_samples += AUDIO_DIAG_SAMPLES_PER_FRAME;

        ret = esp_codec_dev_write(bsp_audio_get_codec(), frame, AUDIO_DIAG_BYTES_PER_FRAME);
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "loopback playback failed on frame %u: %d", (unsigned int)frame_index, ret);
            return ESP_FAIL;
        }

        if (((frame_index + 1) % log_window_frames) == 0 || frame_index + 1 == total_frames) {
            uint32_t in_rms = log_samples > 0 ? integer_sqrt_u64(in_sum_squares / log_samples) : 0;
            uint32_t out_rms = log_samples > 0 ? integer_sqrt_u64(out_sum_squares / log_samples) : 0;
            log_elapsed_ms += log_window_frames * AUDIO_DIAG_FRAME_MS;
            ESP_LOGI(TAG,
                     "loopback window=%u ms samples=%u in_min=%d in_max=%d in_rms=%u out_min=%d out_max=%d out_rms=%u clipped=%u",
                     (unsigned int)log_elapsed_ms,
                     (unsigned int)log_samples,
                     in_min,
                     in_max,
                     (unsigned int)in_rms,
                     out_min,
                     out_max,
                     (unsigned int)out_rms,
                     (unsigned int)clipped_samples);

            in_sum_squares = 0;
            out_sum_squares = 0;
            in_min = INT16_MAX;
            in_max = INT16_MIN;
            out_min = INT16_MAX;
            out_max = INT16_MIN;
            log_samples = 0;
            clipped_samples = 0;
        }
    }

    memset(frame, 0, sizeof(frame));
    (void)esp_codec_dev_write(bsp_audio_get_codec(), frame, AUDIO_DIAG_BYTES_PER_FRAME);
    mute_ret = esp_codec_dev_set_out_mute(bsp_audio_get_codec(), true);
    if (mute_ret != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "mute output after loopback failed: %d", mute_ret);
    }
    ESP_LOGI(TAG, "local loopback finished");
    return ESP_OK;
}
