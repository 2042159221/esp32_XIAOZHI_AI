#include "audio_opus_codec.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_audio_types.h"
#include "esp_opus_dec.h"
#include "esp_opus_enc.h"

static const char *TAG = "audio_opus_codec";

#ifndef CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_BITRATE
#define CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_BITRATE 32000
#endif

#ifndef CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_COMPLEXITY
#define CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_COMPLEXITY 0
#endif

static esp_err_t audio_err_to_esp(esp_audio_err_t err)
{
    switch (err) {
    case ESP_AUDIO_ERR_OK:
        return ESP_OK;
    case ESP_AUDIO_ERR_MEM_LACK:
        return ESP_ERR_NO_MEM;
    case ESP_AUDIO_ERR_INVALID_PARAMETER:
        return ESP_ERR_INVALID_ARG;
    case ESP_AUDIO_ERR_NOT_SUPPORT:
        return ESP_ERR_NOT_SUPPORTED;
    case ESP_AUDIO_ERR_DATA_LACK:
        return ESP_ERR_INVALID_SIZE;
    case ESP_AUDIO_ERR_BUFF_NOT_ENOUGH:
        return ESP_ERR_NO_MEM;
    case ESP_AUDIO_ERR_NOT_FOUND:
        return ESP_ERR_NOT_FOUND;
    default:
        return ESP_FAIL;
    }
}

esp_err_t audio_opus_codec_open(audio_opus_codec_t *codec)
{
    ESP_RETURN_ON_FALSE(codec != NULL, ESP_ERR_INVALID_ARG, TAG, "codec handle is NULL");
    memset(codec, 0, sizeof(*codec));

    esp_opus_enc_config_t enc_cfg = ESP_OPUS_ENC_CONFIG_DEFAULT();
    enc_cfg.sample_rate = AUDIO_OPUS_SAMPLE_RATE;
    enc_cfg.channel = AUDIO_OPUS_CHANNELS;
    enc_cfg.bits_per_sample = AUDIO_OPUS_BITS_PER_SAMPLE;
    enc_cfg.bitrate = CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_BITRATE;
    enc_cfg.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS;
    enc_cfg.application_mode = ESP_OPUS_ENC_APPLICATION_VOIP;
    enc_cfg.complexity = CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_COMPLEXITY;
    enc_cfg.enable_fec = false;
    enc_cfg.enable_dtx = false;
    enc_cfg.enable_vbr = false;

    esp_audio_err_t audio_ret = esp_opus_enc_open(&enc_cfg, sizeof(enc_cfg), &codec->encoder);
    if (audio_ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "open opus encoder failed: %d", audio_ret);
        audio_opus_codec_close(codec);
        return audio_err_to_esp(audio_ret);
    }
    ESP_LOGI(TAG, "opus encoder init OK");

    int enc_in_size = 0;
    int enc_out_size = 0;
    audio_ret = esp_opus_enc_get_frame_size(codec->encoder, &enc_in_size, &enc_out_size);
    if (audio_ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "get opus encoder frame size failed: %d", audio_ret);
        audio_opus_codec_close(codec);
        return audio_err_to_esp(audio_ret);
    }

    if (enc_in_size != AUDIO_OPUS_PCM_FRAME_BYTES || enc_out_size <= 0) {
        ESP_LOGE(TAG, "unexpected opus frame size: pcm=%d opus=%d expected_pcm=%d",
                 enc_in_size,
                 enc_out_size,
                 AUDIO_OPUS_PCM_FRAME_BYTES);
        audio_opus_codec_close(codec);
        return ESP_ERR_INVALID_STATE;
    }

    esp_opus_dec_cfg_t dec_cfg = ESP_OPUS_DEC_CONFIG_DEFAULT();
    dec_cfg.sample_rate = AUDIO_OPUS_SAMPLE_RATE;
    dec_cfg.channel = AUDIO_OPUS_CHANNELS;
    dec_cfg.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_60_MS;
    dec_cfg.self_delimited = false;

    audio_ret = esp_opus_dec_open(&dec_cfg, sizeof(dec_cfg), &codec->decoder);
    if (audio_ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "open opus decoder failed: %d", audio_ret);
        audio_opus_codec_close(codec);
        return audio_err_to_esp(audio_ret);
    }
    ESP_LOGI(TAG, "opus decoder init OK");

    codec->pcm_frame_bytes = (size_t)enc_in_size;
    codec->opus_frame_bytes = (size_t)enc_out_size;
    if (codec->opus_frame_bytes < AUDIO_OPUS_ENCODED_FRAME_CAPACITY_BYTES) {
        codec->opus_frame_bytes = AUDIO_OPUS_ENCODED_FRAME_CAPACITY_BYTES;
    }
    codec->decoded_frame_bytes = AUDIO_OPUS_PCM_FRAME_BYTES;

    ESP_LOGI(TAG,
             "opus codec ready sample_rate=%d channels=%d bits=%d frame_duration_ms=%d pcm_frame_bytes=%u opus_frame_recommended=%d opus_frame_capacity=%u bitrate=%d complexity=%d",
             AUDIO_OPUS_SAMPLE_RATE,
             AUDIO_OPUS_CHANNELS,
             AUDIO_OPUS_BITS_PER_SAMPLE,
             AUDIO_OPUS_FRAME_DURATION_MS,
             (unsigned int)codec->pcm_frame_bytes,
             enc_out_size,
             (unsigned int)codec->opus_frame_bytes,
             CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_BITRATE,
             CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_COMPLEXITY);

    return ESP_OK;
}

void audio_opus_codec_close(audio_opus_codec_t *codec)
{
    if (codec == NULL) {
        return;
    }

    if (codec->encoder != NULL) {
        esp_opus_enc_close(codec->encoder);
    }
    if (codec->decoder != NULL) {
        (void)esp_opus_dec_close(codec->decoder);
    }

    memset(codec, 0, sizeof(*codec));
}

esp_err_t audio_opus_codec_encode(audio_opus_codec_t *codec,
                                  const uint8_t *pcm,
                                  size_t pcm_len,
                                  uint8_t *opus,
                                  size_t opus_capacity,
                                  size_t *opus_len)
{
    ESP_RETURN_ON_FALSE(codec != NULL && codec->encoder != NULL, ESP_ERR_INVALID_STATE, TAG, "opus encoder is not ready");
    ESP_RETURN_ON_FALSE(pcm != NULL && opus != NULL && opus_len != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid opus encode arguments");
    ESP_RETURN_ON_FALSE(pcm_len == codec->pcm_frame_bytes, ESP_ERR_INVALID_SIZE, TAG, "invalid pcm input bytes=%u expected=%u",
                        (unsigned int)pcm_len,
                        (unsigned int)codec->pcm_frame_bytes);
    ESP_RETURN_ON_FALSE(opus_capacity >= codec->opus_frame_bytes, ESP_ERR_INVALID_SIZE, TAG, "opus output buffer too small");

    esp_audio_enc_in_frame_t in_frame = {
        .buffer = (uint8_t *)pcm,
        .len = (uint32_t)pcm_len,
    };
    esp_audio_enc_out_frame_t out_frame = {
        .buffer = opus,
        .len = (uint32_t)opus_capacity,
    };

    esp_audio_err_t audio_ret = esp_opus_enc_process(codec->encoder, &in_frame, &out_frame);
    if (audio_ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "opus encode failed: %d", audio_ret);
        return audio_err_to_esp(audio_ret);
    }

    ESP_RETURN_ON_FALSE(out_frame.encoded_bytes > 0, ESP_FAIL, TAG, "opus encoder produced empty frame");
    *opus_len = out_frame.encoded_bytes;
    return ESP_OK;
}

esp_err_t audio_opus_codec_decode(audio_opus_codec_t *codec,
                                  const uint8_t *opus,
                                  size_t opus_len,
                                  uint8_t *pcm,
                                  size_t pcm_capacity,
                                  size_t *pcm_len)
{
    ESP_RETURN_ON_FALSE(codec != NULL && codec->decoder != NULL, ESP_ERR_INVALID_STATE, TAG, "opus decoder is not ready");
    ESP_RETURN_ON_FALSE(opus != NULL && pcm != NULL && pcm_len != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid opus decode arguments");
    ESP_RETURN_ON_FALSE(opus_len > 0, ESP_ERR_INVALID_SIZE, TAG, "empty opus input");
    ESP_RETURN_ON_FALSE(pcm_capacity >= codec->decoded_frame_bytes, ESP_ERR_INVALID_SIZE, TAG, "decoded pcm buffer too small");

    esp_audio_dec_in_raw_t raw = {
        .buffer = (uint8_t *)opus,
        .len = (uint32_t)opus_len,
    };
    esp_audio_dec_out_frame_t out_frame = {
        .buffer = pcm,
        .len = (uint32_t)pcm_capacity,
    };
    esp_audio_dec_info_t info = {0};

    esp_audio_err_t audio_ret = esp_opus_dec_decode(codec->decoder, &raw, &out_frame, &info);
    if (audio_ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "opus decode failed: %d needed_size=%u consumed=%u",
                 audio_ret,
                 (unsigned int)out_frame.needed_size,
                 (unsigned int)raw.consumed);
        return audio_err_to_esp(audio_ret);
    }

    ESP_RETURN_ON_FALSE(raw.consumed == opus_len, ESP_FAIL, TAG, "opus decoder consumed %u of %u bytes",
                        (unsigned int)raw.consumed,
                        (unsigned int)opus_len);
    ESP_RETURN_ON_FALSE(out_frame.decoded_size == codec->decoded_frame_bytes, ESP_FAIL, TAG, "decoded pcm bytes=%u expected=%u",
                        (unsigned int)out_frame.decoded_size,
                        (unsigned int)codec->decoded_frame_bytes);

    *pcm_len = out_frame.decoded_size;
    return ESP_OK;
}
