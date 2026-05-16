#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_OPUS_SAMPLE_RATE 16000
#define AUDIO_OPUS_CHANNELS 1
#define AUDIO_OPUS_BITS_PER_SAMPLE 16

#ifdef CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_FRAME_DURATION_MS
#define AUDIO_OPUS_FRAME_DURATION_MS CONFIG_XIAOZHI_AUDIO_OPUS_DIAG_FRAME_DURATION_MS
#else
#define AUDIO_OPUS_FRAME_DURATION_MS 60
#endif

#if AUDIO_OPUS_FRAME_DURATION_MS != 20 && AUDIO_OPUS_FRAME_DURATION_MS != 60
#error "AUDIO_OPUS_FRAME_DURATION_MS must be 20 or 60"
#endif

#define AUDIO_OPUS_PCM_FRAME_BYTES \
    ((AUDIO_OPUS_SAMPLE_RATE * AUDIO_OPUS_CHANNELS * (AUDIO_OPUS_BITS_PER_SAMPLE / 8) * AUDIO_OPUS_FRAME_DURATION_MS) / 1000)
#define AUDIO_OPUS_ENCODED_FRAME_CAPACITY_BYTES 2048

typedef struct {
    void *encoder;
    size_t pcm_frame_bytes;
    size_t opus_frame_bytes;
    uint32_t encode_calls;
} audio_opus_encoder_t;

typedef struct {
    void *decoder;
    size_t decoded_frame_bytes;
    uint32_t decode_calls;
    int output_sample_rate;
} audio_opus_decoder_t;

typedef struct {
    void *encoder;
    void *decoder;
    size_t pcm_frame_bytes;
    size_t opus_frame_bytes;
    size_t decoded_frame_bytes;
    uint32_t encode_calls;
    uint32_t decode_calls;
} audio_opus_codec_t;

esp_err_t audio_opus_encoder_open(audio_opus_encoder_t *enc);
void audio_opus_encoder_close(audio_opus_encoder_t *enc);

esp_err_t audio_opus_decoder_open(audio_opus_decoder_t *dec, int output_sample_rate);
void audio_opus_decoder_close(audio_opus_decoder_t *dec);

esp_err_t audio_opus_codec_open(audio_opus_codec_t *codec);
void audio_opus_codec_close(audio_opus_codec_t *codec);

esp_err_t audio_opus_encoder_encode(audio_opus_encoder_t *enc,
                                  const uint8_t *pcm,
                                  size_t pcm_len,
                                  uint8_t *opus,
                                  size_t opus_capacity,
                                  size_t *opus_len);

esp_err_t audio_opus_decoder_decode(audio_opus_decoder_t *dec,
                                  const uint8_t *opus,
                                  size_t opus_len,
                                  uint8_t *pcm,
                                  size_t pcm_capacity,
                                  size_t *pcm_len);

esp_err_t audio_opus_codec_encode(audio_opus_codec_t *codec,
                                  const uint8_t *pcm,
                                  size_t pcm_len,
                                  uint8_t *opus,
                                  size_t opus_capacity,
                                  size_t *opus_len);

esp_err_t audio_opus_codec_decode(audio_opus_codec_t *codec,
                                  const uint8_t *opus,
                                  size_t opus_len,
                                  uint8_t *pcm,
                                  size_t pcm_capacity,
                                  size_t *pcm_len);

#ifdef __cplusplus
}
#endif
