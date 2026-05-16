#include "audio_opus_codec.h"

#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_audio_types.h"
#include "esp_opus_dec.h"
#include "esp_opus_enc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "audio_opus_codec";

#ifndef CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_BITRATE
#define CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_BITRATE 90000
#endif

#ifndef CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_COMPLEXITY
#define CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_COMPLEXITY 0
#endif

static esp_opus_enc_frame_duration_t get_encoder_frame_duration(void)
{
#if AUDIO_OPUS_FRAME_DURATION_MS == 20
    return ESP_OPUS_ENC_FRAME_DURATION_20_MS;
#else
    return ESP_OPUS_ENC_FRAME_DURATION_60_MS;
#endif
}

static esp_opus_dec_frame_duration_t get_decoder_frame_duration(void)
{
#if AUDIO_OPUS_FRAME_DURATION_MS == 20
    return ESP_OPUS_DEC_FRAME_DURATION_20_MS;
#else
    return ESP_OPUS_DEC_FRAME_DURATION_60_MS;
#endif
}

static bool should_log_codec_call(uint32_t call_index)
{
    return call_index <= 3 || (call_index % 16) == 0;
}

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

static void log_codec_heap_state(const char *stage)
{
    ESP_LOGI(TAG,
             "%s task=%s core=%d heap=%u min_free=%u internal_free=%u internal_largest=%u spiram_free=%u spiram_largest=%u stack_watermark=%u",
             stage,
             pcTaskGetName(NULL),
             xPortGetCoreID(),
             (unsigned int)esp_get_free_heap_size(),
             (unsigned int)esp_get_minimum_free_heap_size(),
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned int)uxTaskGetStackHighWaterMark(NULL));
}

static size_t decoded_frame_bytes_for_sample_rate(int sample_rate)
{
    return (size_t)((sample_rate * AUDIO_OPUS_CHANNELS * (AUDIO_OPUS_BITS_PER_SAMPLE / 8) * AUDIO_OPUS_FRAME_DURATION_MS) / 1000);
}

static esp_err_t open_encoder_handle(void **encoder, size_t *pcm_frame_bytes, size_t *opus_frame_bytes)
{
    ESP_RETURN_ON_FALSE(encoder != NULL && pcm_frame_bytes != NULL && opus_frame_bytes != NULL,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "invalid encoder open args");
    *encoder = NULL;
    *pcm_frame_bytes = 0;
    *opus_frame_bytes = 0;

    esp_opus_enc_config_t enc_cfg = ESP_OPUS_ENC_CONFIG_DEFAULT();
    enc_cfg.sample_rate = AUDIO_OPUS_SAMPLE_RATE;
    enc_cfg.channel = AUDIO_OPUS_CHANNELS;
    enc_cfg.bits_per_sample = AUDIO_OPUS_BITS_PER_SAMPLE;
    enc_cfg.bitrate = CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_BITRATE;
    enc_cfg.frame_duration = get_encoder_frame_duration();
    enc_cfg.application_mode = ESP_OPUS_ENC_APPLICATION_AUDIO;
    enc_cfg.complexity = CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_COMPLEXITY;
    enc_cfg.enable_fec = false;
    enc_cfg.enable_dtx = false;
    enc_cfg.enable_vbr = false;

    ESP_LOGI(TAG,
             "opening opus encoder sample_rate=%d channels=%d bits=%d bitrate=%d frame_duration_ms=%d frame_duration_id=%d application=AUDIO complexity=%d fec=%d dtx=%d vbr=%d",
             enc_cfg.sample_rate,
             enc_cfg.channel,
             enc_cfg.bits_per_sample,
             enc_cfg.bitrate,
             AUDIO_OPUS_FRAME_DURATION_MS,
             enc_cfg.frame_duration,
             enc_cfg.complexity,
             enc_cfg.enable_fec,
             enc_cfg.enable_dtx,
             enc_cfg.enable_vbr);
    log_codec_heap_state("before opus encoder open");

    esp_audio_err_t audio_ret = esp_opus_enc_open(&enc_cfg, sizeof(enc_cfg), encoder);
    if (audio_ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "open opus encoder failed: %d encoder=%p", audio_ret, *encoder);
        log_codec_heap_state("after opus encoder open failed");
        return audio_err_to_esp(audio_ret);
    }
    ESP_LOGI(TAG, "opus encoder init OK");
    log_codec_heap_state("after opus encoder open");

    int enc_in_size = 0;
    int enc_out_size = 0;
    audio_ret = esp_opus_enc_get_frame_size(*encoder, &enc_in_size, &enc_out_size);
    if (audio_ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "get opus encoder frame size failed: %d", audio_ret);
        esp_opus_enc_close(*encoder);
        *encoder = NULL;
        return audio_err_to_esp(audio_ret);
    }

    if (enc_in_size != AUDIO_OPUS_PCM_FRAME_BYTES || enc_out_size <= 0) {
        ESP_LOGE(TAG, "unexpected opus frame size: pcm=%d opus=%d expected_pcm=%d",
                 enc_in_size,
                 enc_out_size,
                 AUDIO_OPUS_PCM_FRAME_BYTES);
        esp_opus_enc_close(*encoder);
        *encoder = NULL;
        return ESP_ERR_INVALID_STATE;
    }

    *pcm_frame_bytes = (size_t)enc_in_size;
    *opus_frame_bytes = (size_t)enc_out_size;
    if (*opus_frame_bytes < AUDIO_OPUS_ENCODED_FRAME_CAPACITY_BYTES) {
        *opus_frame_bytes = AUDIO_OPUS_ENCODED_FRAME_CAPACITY_BYTES;
    }

    ESP_LOGI(TAG,
             "opus encoder ready sample_rate=%d channels=%d bits=%d frame_duration_ms=%d pcm_frame_bytes=%u opus_frame_recommended=%d opus_frame_capacity=%u bitrate=%d complexity=%d",
             AUDIO_OPUS_SAMPLE_RATE,
             AUDIO_OPUS_CHANNELS,
             AUDIO_OPUS_BITS_PER_SAMPLE,
             AUDIO_OPUS_FRAME_DURATION_MS,
             (unsigned int)*pcm_frame_bytes,
             enc_out_size,
             (unsigned int)*opus_frame_bytes,
             CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_BITRATE,
             CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_COMPLEXITY);
    return ESP_OK;
}

static esp_err_t open_decoder_handle(void **decoder, int output_sample_rate, size_t *decoded_frame_bytes)
{
    ESP_RETURN_ON_FALSE(decoder != NULL && decoded_frame_bytes != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid decoder open args");
    ESP_RETURN_ON_FALSE(output_sample_rate > 0, ESP_ERR_INVALID_ARG, TAG, "invalid decoder sample rate=%d", output_sample_rate);
    *decoder = NULL;
    *decoded_frame_bytes = 0;

    esp_opus_dec_cfg_t dec_cfg = ESP_OPUS_DEC_CONFIG_DEFAULT();
    dec_cfg.sample_rate = output_sample_rate;
    dec_cfg.channel = AUDIO_OPUS_CHANNELS;
    dec_cfg.frame_duration = get_decoder_frame_duration();
    dec_cfg.self_delimited = false;

    ESP_LOGI(TAG,
             "opening opus decoder output_sample_rate=%u channels=%u frame_duration_ms=%u frame_duration_id=%u",
             (unsigned int)dec_cfg.sample_rate,
             (unsigned int)dec_cfg.channel,
             (unsigned int)AUDIO_OPUS_FRAME_DURATION_MS,
             (unsigned int)dec_cfg.frame_duration);
    log_codec_heap_state("before opus decoder open");

    esp_audio_err_t audio_ret = esp_opus_dec_open(&dec_cfg, sizeof(dec_cfg), decoder);
    if (audio_ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "open opus decoder failed: %d", audio_ret);
        log_codec_heap_state("after opus decoder open failed");
        return audio_err_to_esp(audio_ret);
    }
    ESP_LOGI(TAG, "opus decoder init OK");
    log_codec_heap_state("after opus decoder open");

    ESP_LOGI(TAG,
             "opus decoder ready output_sample_rate=%d decoded_frame_bytes=%u",
             output_sample_rate,
             (unsigned int)decoded_frame_bytes_for_sample_rate(output_sample_rate));
    *decoded_frame_bytes = decoded_frame_bytes_for_sample_rate(output_sample_rate);
    return ESP_OK;
}

esp_err_t audio_opus_encoder_open(audio_opus_encoder_t *enc)
{
    ESP_RETURN_ON_FALSE(enc != NULL, ESP_ERR_INVALID_ARG, TAG, "encoder handle is NULL");
    memset(enc, 0, sizeof(*enc));
    return open_encoder_handle(&enc->encoder, &enc->pcm_frame_bytes, &enc->opus_frame_bytes);
}

void audio_opus_encoder_close(audio_opus_encoder_t *enc)
{
    if (enc == NULL) {
        return;
    }
    if (enc->encoder != NULL) {
        esp_opus_enc_close(enc->encoder);
    }
    memset(enc, 0, sizeof(*enc));
}

esp_err_t audio_opus_decoder_open(audio_opus_decoder_t *dec, int output_sample_rate)
{
    ESP_RETURN_ON_FALSE(dec != NULL, ESP_ERR_INVALID_ARG, TAG, "decoder handle is NULL");
    memset(dec, 0, sizeof(*dec));

    esp_err_t err = open_decoder_handle(&dec->decoder, output_sample_rate, &dec->decoded_frame_bytes);
    if (err == ESP_OK) {
        dec->output_sample_rate = output_sample_rate;
    }
    return err;
}

void audio_opus_decoder_close(audio_opus_decoder_t *dec)
{
    if (dec == NULL) {
        return;
    }
    if (dec->decoder != NULL) {
        (void)esp_opus_dec_close(dec->decoder);
    }
    memset(dec, 0, sizeof(*dec));
}

esp_err_t audio_opus_codec_open(audio_opus_codec_t *codec)
{
    ESP_RETURN_ON_FALSE(codec != NULL, ESP_ERR_INVALID_ARG, TAG, "codec handle is NULL");
    memset(codec, 0, sizeof(*codec));

    esp_err_t err = open_encoder_handle(&codec->encoder, &codec->pcm_frame_bytes, &codec->opus_frame_bytes);
    if (err != ESP_OK) {
        audio_opus_codec_close(codec);
        return err;
    }

    err = open_decoder_handle(&codec->decoder, AUDIO_OPUS_SAMPLE_RATE, &codec->decoded_frame_bytes);
    if (err != ESP_OK) {
        audio_opus_codec_close(codec);
        return err;
    }

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

static esp_err_t encode_with_handle(void *encoder,
                                    size_t pcm_frame_bytes,
                                    size_t opus_frame_bytes,
                                    uint32_t *encode_calls,
                                    const uint8_t *pcm,
                                    size_t pcm_len,
                                    uint8_t *opus,
                                    size_t opus_capacity,
                                    size_t *opus_len)
{
    ESP_RETURN_ON_FALSE(encoder != NULL, ESP_ERR_INVALID_STATE, TAG, "opus encoder is not ready");
    ESP_RETURN_ON_FALSE(pcm != NULL && opus != NULL && opus_len != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid opus encode arguments");
    ESP_RETURN_ON_FALSE(pcm_len == pcm_frame_bytes, ESP_ERR_INVALID_SIZE, TAG, "invalid pcm input bytes=%u expected=%u",
                        (unsigned int)pcm_len,
                        (unsigned int)pcm_frame_bytes);
    ESP_RETURN_ON_FALSE(opus_capacity >= opus_frame_bytes, ESP_ERR_INVALID_SIZE, TAG, "opus output buffer too small");

    esp_audio_enc_in_frame_t in_frame = {
        .buffer = (uint8_t *)pcm,
        .len = (uint32_t)pcm_len,
    };
    esp_audio_enc_out_frame_t out_frame = {
        .buffer = opus,
        .len = (uint32_t)opus_capacity,
    };

    uint32_t local_calls = 0;
    uint32_t *calls = encode_calls != NULL ? encode_calls : &local_calls;
    (*calls)++;
    const uint32_t call_index = *calls;
    if (should_log_codec_call(call_index)) {
        ESP_LOGI(TAG,
                 "before opus encode call=%u pcm input bytes=%u opus capacity bytes=%u encoder=%p free heap=%u internal free=%u minimum free heap=%u task stack watermark=%u",
                 (unsigned int)call_index,
                 (unsigned int)pcm_len,
                 (unsigned int)opus_capacity,
                 encoder,
                 (unsigned int)esp_get_free_heap_size(),
                 (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned int)esp_get_minimum_free_heap_size(),
                 (unsigned int)uxTaskGetStackHighWaterMark(NULL));
    }

    esp_audio_err_t audio_ret = esp_opus_enc_process(encoder, &in_frame, &out_frame);
    if (audio_ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "opus encode failed: %d", audio_ret);
        return audio_err_to_esp(audio_ret);
    }

    ESP_RETURN_ON_FALSE(out_frame.encoded_bytes > 0, ESP_FAIL, TAG, "opus encoder produced empty frame");
    ESP_RETURN_ON_FALSE(out_frame.encoded_bytes <= opus_capacity, ESP_FAIL, TAG, "opus encoder overflow encoded=%u capacity=%u",
                        (unsigned int)out_frame.encoded_bytes,
                        (unsigned int)opus_capacity);
    if (should_log_codec_call(call_index)) {
        ESP_LOGI(TAG,
                 "after opus encode call=%u opus encoded bytes=%u free heap=%u internal free=%u task stack watermark=%u",
                 (unsigned int)call_index,
                 (unsigned int)out_frame.encoded_bytes,
                 (unsigned int)esp_get_free_heap_size(),
                 (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned int)uxTaskGetStackHighWaterMark(NULL));
    }
    *opus_len = out_frame.encoded_bytes;
    return ESP_OK;
}

static esp_err_t decode_with_handle(void *decoder,
                                    size_t decoded_frame_bytes,
                                    uint32_t *decode_calls,
                                    const uint8_t *opus,
                                    size_t opus_len,
                                    uint8_t *pcm,
                                    size_t pcm_capacity,
                                    size_t *pcm_len)
{
    ESP_RETURN_ON_FALSE(decoder != NULL, ESP_ERR_INVALID_STATE, TAG, "opus decoder is not ready");
    ESP_RETURN_ON_FALSE(opus != NULL && pcm != NULL && pcm_len != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid opus decode arguments");
    ESP_RETURN_ON_FALSE(opus_len > 0, ESP_ERR_INVALID_SIZE, TAG, "empty opus input");
    ESP_RETURN_ON_FALSE(pcm_capacity >= decoded_frame_bytes, ESP_ERR_INVALID_SIZE, TAG, "decoded pcm buffer too small");

    esp_audio_dec_in_raw_t raw = {
        .buffer = (uint8_t *)opus,
        .len = (uint32_t)opus_len,
    };
    esp_audio_dec_out_frame_t out_frame = {
        .buffer = pcm,
        .len = (uint32_t)pcm_capacity,
    };
    esp_audio_dec_info_t info = {0};

    uint32_t local_calls = 0;
    uint32_t *calls = decode_calls != NULL ? decode_calls : &local_calls;
    (*calls)++;
    const uint32_t call_index = *calls;
    if (should_log_codec_call(call_index)) {
        ESP_LOGI(TAG,
                 "before opus decode call=%u opus input bytes=%u pcm capacity bytes=%u decoder=%p free heap=%u internal free=%u task stack watermark=%u",
                 (unsigned int)call_index,
                 (unsigned int)opus_len,
                 (unsigned int)pcm_capacity,
                 decoder,
                 (unsigned int)esp_get_free_heap_size(),
                 (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned int)uxTaskGetStackHighWaterMark(NULL));
    }

    esp_audio_err_t audio_ret = esp_opus_dec_decode(decoder, &raw, &out_frame, &info);
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
    ESP_RETURN_ON_FALSE(out_frame.decoded_size == decoded_frame_bytes, ESP_FAIL, TAG, "decoded pcm bytes=%u expected=%u",
                        (unsigned int)out_frame.decoded_size,
                        (unsigned int)decoded_frame_bytes);

    if (should_log_codec_call(call_index)) {
        ESP_LOGI(TAG,
                 "after opus decode call=%u decoded pcm bytes=%u consumed bytes=%u free heap=%u internal free=%u task stack watermark=%u",
                 (unsigned int)call_index,
                 (unsigned int)out_frame.decoded_size,
                 (unsigned int)raw.consumed,
                 (unsigned int)esp_get_free_heap_size(),
                 (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned int)uxTaskGetStackHighWaterMark(NULL));
    }
    *pcm_len = out_frame.decoded_size;
    return ESP_OK;
}

esp_err_t audio_opus_encoder_encode(audio_opus_encoder_t *enc,
                                    const uint8_t *pcm,
                                    size_t pcm_len,
                                    uint8_t *opus,
                                    size_t opus_capacity,
                                    size_t *opus_len)
{
    ESP_RETURN_ON_FALSE(enc != NULL, ESP_ERR_INVALID_ARG, TAG, "encoder handle is NULL");
    return encode_with_handle(enc->encoder,
                              enc->pcm_frame_bytes,
                              enc->opus_frame_bytes,
                              &enc->encode_calls,
                              pcm,
                              pcm_len,
                              opus,
                              opus_capacity,
                              opus_len);
}

esp_err_t audio_opus_decoder_decode(audio_opus_decoder_t *dec,
                                    const uint8_t *opus,
                                    size_t opus_len,
                                    uint8_t *pcm,
                                    size_t pcm_capacity,
                                    size_t *pcm_len)
{
    ESP_RETURN_ON_FALSE(dec != NULL, ESP_ERR_INVALID_ARG, TAG, "decoder handle is NULL");
    return decode_with_handle(dec->decoder,
                              dec->decoded_frame_bytes,
                              &dec->decode_calls,
                              opus,
                              opus_len,
                              pcm,
                              pcm_capacity,
                              pcm_len);
}

esp_err_t audio_opus_codec_encode(audio_opus_codec_t *codec,
                                  const uint8_t *pcm,
                                  size_t pcm_len,
                                  uint8_t *opus,
                                  size_t opus_capacity,
                                  size_t *opus_len)
{
    ESP_RETURN_ON_FALSE(codec != NULL, ESP_ERR_INVALID_ARG, TAG, "codec handle is NULL");
    return encode_with_handle(codec->encoder,
                              codec->pcm_frame_bytes,
                              codec->opus_frame_bytes,
                              &codec->encode_calls,
                              pcm,
                              pcm_len,
                              opus,
                              opus_capacity,
                              opus_len);
}

esp_err_t audio_opus_codec_decode(audio_opus_codec_t *codec,
                                  const uint8_t *opus,
                                  size_t opus_len,
                                  uint8_t *pcm,
                                  size_t pcm_capacity,
                                  size_t *pcm_len)
{
    ESP_RETURN_ON_FALSE(codec != NULL, ESP_ERR_INVALID_ARG, TAG, "codec handle is NULL");
    return decode_with_handle(codec->decoder,
                              codec->decoded_frame_bytes,
                              &codec->decode_calls,
                              opus,
                              opus_len,
                              pcm,
                              pcm_capacity,
                              pcm_len);
}
