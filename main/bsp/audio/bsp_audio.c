#include "bsp_audio.h"

#include <stdbool.h>

#include "board_pins.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "bsp_audio";

static i2c_master_bus_handle_t s_i2c_bus;
static i2s_chan_handle_t s_i2s_tx_chan;
static i2s_chan_handle_t s_i2s_rx_chan;
static const audio_codec_data_if_t *s_i2s_data_if;
static esp_codec_dev_handle_t s_codec;
static bool s_codec_opened;
static int s_current_sample_rate = BSP_AUDIO_SAMPLE_RATE;
static SemaphoreHandle_t s_audio_lock;
static portMUX_TYPE s_audio_lock_mux = portMUX_INITIALIZER_UNLOCKED;

static esp_err_t bsp_audio_init_locked(void);
static esp_err_t bsp_audio_open_with_sample_rate_locked(int sample_rate);

static esp_err_t ensure_audio_lock(void)
{
    if (s_audio_lock != NULL) {
        return ESP_OK;
    }

    SemaphoreHandle_t created_lock = xSemaphoreCreateMutex();
    if (created_lock == NULL) {
        ESP_RETURN_ON_FALSE(s_audio_lock != NULL, ESP_ERR_NO_MEM, TAG, "create audio mutex failed");
        return ESP_OK;
    }

    portENTER_CRITICAL(&s_audio_lock_mux);
    if (s_audio_lock == NULL) {
        s_audio_lock = created_lock;
        created_lock = NULL;
    }
    portEXIT_CRITICAL(&s_audio_lock_mux);

    if (created_lock != NULL) {
        vSemaphoreDelete(created_lock);
    }
    return ESP_OK;
}

static esp_err_t probe_es8311_addr(uint8_t *addr)
{
    ESP_RETURN_ON_FALSE(addr != NULL, ESP_ERR_INVALID_ARG, TAG, "ES8311 address output is NULL");

    ESP_LOGI(TAG, "Start ES8311 I2C probe on SDA=%d SCL=%d", BOARD_AUDIO_I2C_SDA, BOARD_AUDIO_I2C_SCL);

    const uint8_t candidates[] = {0x18, 0x19};
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        esp_err_t err = i2c_master_probe(s_i2c_bus, candidates[i], 100);
        if (err == ESP_OK) {
            *addr = candidates[i];
            ESP_LOGI(TAG, "ES8311 probe result: case %c, 7-bit address 0x%02X, codec address 0x%02X",
                     *addr == 0x18 ? 'A' : 'B', *addr, (uint8_t)(*addr << 1));
            return ESP_OK;
        }
        ESP_LOGW(TAG, "ES8311 not detected at I2C address 0x%02X: %s", candidates[i], esp_err_to_name(err));
    }

    ESP_LOGE(TAG, "ES8311 not detected at 0x18 or 0x19; check CCLK/CDATA, CE level, power, and reset state");
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t init_i2c(void)
{
    if (s_i2c_bus != NULL) {
        return ESP_OK;
    }

    const i2c_master_bus_config_t i2c_config = {
        .i2c_port = BOARD_AUDIO_I2C_PORT,
        .sda_io_num = BOARD_AUDIO_I2C_SDA,
        .scl_io_num = BOARD_AUDIO_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&i2c_config, &s_i2c_bus);
}

static esp_err_t init_i2s(void)
{
    if (s_i2s_tx_chan != NULL && s_i2s_rx_chan != NULL && s_i2s_data_if != NULL) {
        return ESP_OK;
    }

    const i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(BOARD_AUDIO_I2S_PORT, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_i2s_tx_chan, &s_i2s_rx_chan), TAG, "create I2S channels failed");

    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BSP_AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = BOARD_AUDIO_I2S_MCK,
            .bclk = BOARD_AUDIO_I2S_BCK,
            .ws = BOARD_AUDIO_I2S_WS,
            .dout = BOARD_AUDIO_I2S_DO,
            .din = BOARD_AUDIO_I2S_DI,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    esp_err_t err = i2s_channel_init_std_mode(s_i2s_tx_chan, &std_cfg);
    if (err == ESP_OK) {
        err = i2s_channel_init_std_mode(s_i2s_rx_chan, &std_cfg);
    }
    if (err == ESP_OK) {
        err = i2s_channel_enable(s_i2s_tx_chan);
    }
    if (err == ESP_OK) {
        err = i2s_channel_enable(s_i2s_rx_chan);
    }
    if (err != ESP_OK) {
        if (s_i2s_tx_chan != NULL) {
            i2s_del_channel(s_i2s_tx_chan);
            s_i2s_tx_chan = NULL;
        }
        if (s_i2s_rx_chan != NULL) {
            i2s_del_channel(s_i2s_rx_chan);
            s_i2s_rx_chan = NULL;
        }
        return err;
    }

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = BOARD_AUDIO_I2S_PORT,
        .rx_handle = s_i2s_rx_chan,
        .tx_handle = s_i2s_tx_chan,
    };
    s_i2s_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    ESP_RETURN_ON_FALSE(s_i2s_data_if != NULL, ESP_ERR_NO_MEM, TAG, "create codec I2S data interface failed");

    return ESP_OK;
}

static bool is_supported_sample_rate(int sample_rate)
{
    return sample_rate == 16000 || sample_rate == 24000;
}

static void restore_i2s_sample_rate(int old_sample_rate)
{
    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(old_sample_rate);

    esp_err_t tx_disable = i2s_channel_disable(s_i2s_tx_chan);
    if (tx_disable != ESP_OK && tx_disable != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "disable I2S TX for restore failed: %s", esp_err_to_name(tx_disable));
    }
    esp_err_t rx_disable = i2s_channel_disable(s_i2s_rx_chan);
    if (rx_disable != ESP_OK && rx_disable != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "disable I2S RX for restore failed: %s", esp_err_to_name(rx_disable));
    }

    esp_err_t tx_reconfig = i2s_channel_reconfig_std_clock(s_i2s_tx_chan, &clk_cfg);
    if (tx_reconfig != ESP_OK) {
        ESP_LOGE(TAG, "restore I2S TX clock to %d Hz failed: %s", old_sample_rate, esp_err_to_name(tx_reconfig));
    }
    esp_err_t rx_reconfig = i2s_channel_reconfig_std_clock(s_i2s_rx_chan, &clk_cfg);
    if (rx_reconfig != ESP_OK) {
        ESP_LOGE(TAG, "restore I2S RX clock to %d Hz failed: %s", old_sample_rate, esp_err_to_name(rx_reconfig));
    }

    esp_err_t tx_enable = i2s_channel_enable(s_i2s_tx_chan);
    if (tx_enable != ESP_OK) {
        ESP_LOGE(TAG, "restore I2S TX enable failed: %s", esp_err_to_name(tx_enable));
    }
    esp_err_t rx_enable = i2s_channel_enable(s_i2s_rx_chan);
    if (rx_enable != ESP_OK) {
        ESP_LOGE(TAG, "restore I2S RX enable failed: %s", esp_err_to_name(rx_enable));
    }
}

static esp_err_t reconfigure_i2s_sample_rate(int old_sample_rate, int sample_rate)
{
    ESP_RETURN_ON_FALSE(s_i2s_tx_chan != NULL && s_i2s_rx_chan != NULL, ESP_ERR_INVALID_STATE, TAG, "I2S channels are not ready");
    ESP_RETURN_ON_FALSE(is_supported_sample_rate(old_sample_rate), ESP_ERR_INVALID_ARG, TAG, "unsupported old audio sample rate=%d", old_sample_rate);
    ESP_RETURN_ON_FALSE(is_supported_sample_rate(sample_rate), ESP_ERR_INVALID_ARG, TAG, "unsupported audio sample rate=%d", sample_rate);

    if (old_sample_rate == sample_rate) {
        return ESP_OK;
    }

    esp_err_t tx_disable = i2s_channel_disable(s_i2s_tx_chan);
    esp_err_t rx_disable = i2s_channel_disable(s_i2s_rx_chan);
    if (tx_disable != ESP_OK && tx_disable != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "disable I2S TX before sample-rate switch failed: %s", esp_err_to_name(tx_disable));
        restore_i2s_sample_rate(old_sample_rate);
        return tx_disable;
    }
    if (rx_disable != ESP_OK && rx_disable != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "disable I2S RX before sample-rate switch failed: %s", esp_err_to_name(rx_disable));
        restore_i2s_sample_rate(old_sample_rate);
        return rx_disable;
    }

    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    esp_err_t err = i2s_channel_reconfig_std_clock(s_i2s_tx_chan, &clk_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "reconfig I2S TX clock to %d Hz failed: %s", sample_rate, esp_err_to_name(err));
        restore_i2s_sample_rate(old_sample_rate);
        return err;
    }
    err = i2s_channel_reconfig_std_clock(s_i2s_rx_chan, &clk_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "reconfig I2S RX clock to %d Hz failed: %s", sample_rate, esp_err_to_name(err));
        restore_i2s_sample_rate(old_sample_rate);
        return err;
    }
    err = i2s_channel_enable(s_i2s_tx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "enable I2S TX at %d Hz failed: %s", sample_rate, esp_err_to_name(err));
        restore_i2s_sample_rate(old_sample_rate);
        return err;
    }
    err = i2s_channel_enable(s_i2s_rx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "enable I2S RX at %d Hz failed: %s", sample_rate, esp_err_to_name(err));
        restore_i2s_sample_rate(old_sample_rate);
        return err;
    }

    ESP_LOGI(TAG, "I2S sample rate reconfigured to %d Hz", sample_rate);
    return ESP_OK;
}

static esp_err_t bsp_audio_init_locked(void)
{
    if (s_codec != NULL) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(init_i2c(), TAG, "init audio I2C failed");
    ESP_RETURN_ON_ERROR(init_i2s(), TAG, "init audio I2S failed");

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(gpio_if != NULL, ESP_ERR_NO_MEM, TAG, "create codec GPIO interface failed");

    uint8_t es8311_addr = BOARD_AUDIO_ES8311_ADDR;
    ESP_RETURN_ON_ERROR(probe_es8311_addr(&es8311_addr), TAG, "probe ES8311 I2C address failed");

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = BOARD_AUDIO_I2C_PORT,
        .addr = (uint8_t)(es8311_addr << 1),
        .bus_handle = s_i2c_bus,
    };
    const audio_codec_ctrl_if_t *i2c_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(i2c_ctrl_if != NULL, ESP_ERR_NO_MEM, TAG, "create codec I2C control interface failed");

    const esp_codec_dev_hw_gain_t gain = {
        .pa_voltage = 5.0,
        .codec_dac_voltage = 3.3,
    };
    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = i2c_ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = BOARD_AUDIO_PA_EN,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = gain,
    };
    const audio_codec_if_t *es8311_dev = es8311_codec_new(&es8311_cfg);
    ESP_RETURN_ON_FALSE(es8311_dev != NULL, ESP_ERR_NO_MEM, TAG, "create ES8311 codec failed");

    esp_codec_dev_cfg_t codec_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = es8311_dev,
        .data_if = s_i2s_data_if,
    };
    s_codec = esp_codec_dev_new(&codec_dev_cfg);
    ESP_RETURN_ON_FALSE(s_codec != NULL, ESP_ERR_NO_MEM, TAG, "create codec device failed");

    return ESP_OK;
}

esp_err_t bsp_audio_init(void)
{
    ESP_RETURN_ON_ERROR(ensure_audio_lock(), TAG, "create audio mutex failed");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_audio_lock, pdMS_TO_TICKS(1000)) == pdTRUE,
                        ESP_ERR_TIMEOUT,
                        TAG,
                        "take audio mutex failed");
    esp_err_t err = bsp_audio_init_locked();
    xSemaphoreGive(s_audio_lock);
    return err;
}

esp_codec_dev_handle_t bsp_audio_get_codec(void)
{
    return s_codec;
}

esp_err_t bsp_audio_init_i2c_bus(void)
{
    return init_i2c();
}

i2c_master_bus_handle_t bsp_audio_get_i2c_bus(void)
{
    return s_i2c_bus;
}

static esp_err_t bsp_audio_open_with_sample_rate_locked(int sample_rate)
{
    ESP_RETURN_ON_FALSE(is_supported_sample_rate(sample_rate), ESP_ERR_INVALID_ARG, TAG, "unsupported codec sample rate=%d", sample_rate);
    ESP_RETURN_ON_ERROR(bsp_audio_init_locked(), TAG, "init codec before open failed");

    const bool had_open_codec = s_codec_opened;
    const int old_sample_rate = is_supported_sample_rate(s_current_sample_rate) ? s_current_sample_rate : BSP_AUDIO_SAMPLE_RATE;
    esp_codec_dev_sample_info_t old_fs = {
        .sample_rate = old_sample_rate,
        .channel = BSP_AUDIO_CHANNELS,
        .bits_per_sample = BSP_AUDIO_BITS_PER_SAMPLE,
    };

    if (s_codec_opened && old_sample_rate == sample_rate) {
        return ESP_OK;
    }

    if (s_codec_opened) {
        int mute_ret = esp_codec_dev_set_out_mute(s_codec, true);
        if (mute_ret != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "mute before sample-rate switch failed: %d", mute_ret);
        }
        int close_ret = esp_codec_dev_close(s_codec);
        ESP_RETURN_ON_FALSE(close_ret == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "close codec before sample-rate switch failed: %d", close_ret);
        s_codec_opened = false;
    }

    esp_err_t err = reconfigure_i2s_sample_rate(old_sample_rate, sample_rate);
    if (err != ESP_OK) {
        if (had_open_codec) {
            int reopen_ret = esp_codec_dev_open(s_codec, &old_fs);
            if (reopen_ret == ESP_CODEC_DEV_OK) {
                s_codec_opened = true;
                s_current_sample_rate = old_sample_rate;
                ESP_LOGW(TAG, "codec restored to previous sample_rate=%d after I2S switch failure", old_sample_rate);
            } else {
                ESP_LOGE(TAG,
                         "restore codec to sample_rate=%d after I2S switch failure failed: %d; audio path needs restart/retry",
                         old_sample_rate,
                         reopen_ret);
            }
        }
        return err;
    }

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = sample_rate,
        .channel = BSP_AUDIO_CHANNELS,
        .bits_per_sample = BSP_AUDIO_BITS_PER_SAMPLE,
    };
    int ret = esp_codec_dev_open(s_codec, &fs);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "open codec sample_rate=%d failed: %d", sample_rate, ret);
        esp_err_t restore_err = reconfigure_i2s_sample_rate(sample_rate, old_sample_rate);
        if (restore_err != ESP_OK) {
            ESP_LOGE(TAG,
                     "restore I2S sample rate to %d Hz after codec open failure failed: %s; audio path needs restart/retry",
                     old_sample_rate,
                     esp_err_to_name(restore_err));
            s_codec_opened = false;
            return ESP_FAIL;
        }

        if (had_open_codec) {
            int reopen_ret = esp_codec_dev_open(s_codec, &old_fs);
            if (reopen_ret == ESP_CODEC_DEV_OK) {
                s_codec_opened = true;
                s_current_sample_rate = old_sample_rate;
                ESP_LOGW(TAG, "codec restored to previous sample_rate=%d after open failure", old_sample_rate);
            } else {
                ESP_LOGE(TAG,
                         "restore codec to sample_rate=%d after open failure failed: %d; audio path needs restart/retry",
                         old_sample_rate,
                         reopen_ret);
                s_codec_opened = false;
            }
        }
        return ESP_FAIL;
    }

    s_codec_opened = true;
    s_current_sample_rate = sample_rate;
    ESP_LOGI(TAG, "codec opened sample_rate=%d channels=%d bits=%d", sample_rate, BSP_AUDIO_CHANNELS, BSP_AUDIO_BITS_PER_SAMPLE);
    return ESP_OK;
}

esp_err_t bsp_audio_open_with_sample_rate(int sample_rate)
{
    ESP_RETURN_ON_ERROR(ensure_audio_lock(), TAG, "create audio mutex failed");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_audio_lock, pdMS_TO_TICKS(1000)) == pdTRUE,
                        ESP_ERR_TIMEOUT,
                        TAG,
                        "take audio mutex failed");
    esp_err_t err = bsp_audio_open_with_sample_rate_locked(sample_rate);
    xSemaphoreGive(s_audio_lock);
    return err;
}

esp_err_t bsp_audio_open(void)
{
    ESP_RETURN_ON_ERROR(ensure_audio_lock(), TAG, "create audio mutex failed");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_audio_lock, pdMS_TO_TICKS(1000)) == pdTRUE,
                        ESP_ERR_TIMEOUT,
                        TAG,
                        "take audio mutex failed");
    if (s_codec_opened) {
        xSemaphoreGive(s_audio_lock);
        return ESP_OK;
    }

    esp_err_t err = bsp_audio_open_with_sample_rate_locked(BSP_AUDIO_SAMPLE_RATE);
    xSemaphoreGive(s_audio_lock);
    return err;
}

int bsp_audio_get_current_sample_rate(void)
{
    return s_current_sample_rate;
}

esp_err_t bsp_audio_set_volume(int volume)
{
    ESP_RETURN_ON_ERROR(ensure_audio_lock(), TAG, "create audio mutex failed");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_audio_lock, pdMS_TO_TICKS(1000)) == pdTRUE,
                        ESP_ERR_TIMEOUT,
                        TAG,
                        "take audio mutex failed");

    esp_err_t err = ESP_OK;
    if (!s_codec_opened) {
        err = bsp_audio_open_with_sample_rate_locked(BSP_AUDIO_SAMPLE_RATE);
    }
    if (err == ESP_OK) {
        int ret = esp_codec_dev_set_out_vol(s_codec, volume);
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "set volume failed: %d", ret);
            err = ESP_FAIL;
        }
    }

    xSemaphoreGive(s_audio_lock);
    return err;
}
