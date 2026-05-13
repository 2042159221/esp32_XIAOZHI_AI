#include "bsp_audio.h"

#include <stdbool.h>

#include "board_config.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"

static const char *TAG = "bsp_audio";

static i2c_master_bus_handle_t s_i2c_bus;
static i2s_chan_handle_t s_i2s_tx_chan;
static i2s_chan_handle_t s_i2s_rx_chan;
static const audio_codec_data_if_t *s_i2s_data_if;
static esp_codec_dev_handle_t s_codec;
static bool s_codec_opened;

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

esp_err_t bsp_audio_init(void)
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

esp_codec_dev_handle_t bsp_audio_get_codec(void)
{
    return s_codec;
}

esp_err_t bsp_audio_open(void)
{
    ESP_RETURN_ON_ERROR(bsp_audio_init(), TAG, "init codec before open failed");
    if (s_codec_opened) {
        return ESP_OK;
    }

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = BSP_AUDIO_SAMPLE_RATE,
        .channel = BSP_AUDIO_CHANNELS,
        .bits_per_sample = BSP_AUDIO_BITS_PER_SAMPLE,
    };
    int ret = esp_codec_dev_open(s_codec, &fs);
    ESP_RETURN_ON_FALSE(ret == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "open codec failed: %d", ret);
    s_codec_opened = true;
    return ESP_OK;
}

esp_err_t bsp_audio_set_volume(int volume)
{
    ESP_RETURN_ON_ERROR(bsp_audio_open(), TAG, "open codec before set volume failed");
    int ret = esp_codec_dev_set_out_vol(s_codec, volume);
    ESP_RETURN_ON_FALSE(ret == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "set volume failed: %d", ret);
    return ESP_OK;
}