#!/usr/bin/env python3
"""Static P0 voice loop guardrails for the ESP-IDF app."""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(rel_path: str) -> str:
    return (ROOT / rel_path).read_text(encoding="utf-8")


def require(condition: bool, message: str, failures: list[str]) -> None:
    if not condition:
        failures.append(message)


def function_body(source: str, name: str) -> str:
    match = re.search(rf"\b{name}\s*\([^)]*\)\s*\{{", source)
    if match is None:
        return ""

    depth = 0
    start = match.end() - 1
    for index in range(start, len(source)):
        char = source[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[start + 1:index]
    return ""


def check_sdkconfig_defaults(failures: list[str]) -> None:
    defaults = read("sdkconfig.defaults")
    require("CONFIG_SPIRAM_USE_MALLOC=y" in defaults, "sdkconfig.defaults must enable CONFIG_SPIRAM_USE_MALLOC=y", failures)
    require("# CONFIG_SPIRAM_USE_CAPS_ALLOC is not set" in defaults,
            "sdkconfig.defaults must disable CONFIG_SPIRAM_USE_CAPS_ALLOC", failures)
    require("CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=2048" in defaults,
            "sdkconfig.defaults must set CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=2048", failures)
    require("CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=65536" in defaults,
            "sdkconfig.defaults must reserve 65536 internal bytes", failures)
    require("CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y" in defaults,
            "sdkconfig.defaults must allow external task stacks for WithCaps tasks", failures)


def check_codec_split(failures: list[str]) -> None:
    header = read("main/services/audio/audio_opus_codec.h")
    source = read("main/services/audio/audio_opus_codec.c")

    for symbol in (
        "audio_opus_encoder_open",
        "audio_opus_encoder_close",
        "audio_opus_decoder_open",
        "audio_opus_decoder_close",
    ):
        require(symbol in header, f"audio_opus_codec.h must declare {symbol}", failures)
        require(symbol in source, f"audio_opus_codec.c must define {symbol}", failures)

    encoder_body = function_body(source, "audio_opus_encoder_open")
    require("esp_opus_dec_open" not in encoder_body,
            "audio_opus_encoder_open must not open the decoder", failures)


def check_stream_direct_capture(failures: list[str]) -> None:
    header = read("main/services/audio/audio_opus_stream.h")
    source = read("main/services/audio/audio_opus_stream.c")

    require("AUDIO_OPUS_PCM_SOURCE_DIRECT_CODEC" in header,
            "audio_opus_stream.h must expose AUDIO_OPUS_PCM_SOURCE_DIRECT_CODEC", failures)
    require("pcm_source" in header,
            "audio_opus_stream_config_t must include a pcm_source field", failures)
    require("direct_capture_task" in source,
            "audio_opus_stream.c must implement a direct_capture_task", failures)
    require("esp_codec_dev_read" in source,
            "direct capture path must read PCM from the codec", failures)
    require("audio_opus_stream_feed_pcm" in source,
            "direct capture path must feed PCM through audio_opus_stream_feed_pcm", failures)

    stop_body = function_body(source, "audio_opus_stream_stop")
    require("delete_stream_task(" in stop_body,
            "audio_opus_stream_stop must use delete_stream_task for created tasks", failures)
    require("vTaskDelete(s_stream.encoder_task)" not in stop_body,
            "audio_opus_stream_stop must not directly vTaskDelete encoder_task", failures)
    require("vTaskDelete(s_stream.decoder_task)" not in stop_body,
            "audio_opus_stream_stop must not directly vTaskDelete decoder_task", failures)


def check_downlink_sample_rate(failures: list[str]) -> None:
    ws_source = read("main/xiaozhi/xiaozhi_ws.c")
    stream_source = read("main/services/audio/audio_opus_stream.c")
    bsp_header = read("main/bsp/audio/bsp_audio.h")
    bsp_source = read("main/bsp/audio/bsp_audio.c")

    require("resolve_decoder_output_sample_rate" in ws_source,
            "xiaozhi_ws.c must resolve decoder sample rate from server hello", failures)
    require(".decoder_output_sample_rate = resolve_decoder_output_sample_rate()" in ws_source,
            "start_audio_stream must pass server sample rate into audio_opus_stream", failures)
    require("bsp_audio_open_with_sample_rate" in bsp_header,
            "bsp_audio.h must expose bsp_audio_open_with_sample_rate", failures)
    require("i2s_channel_reconfig_std_clock" in bsp_source,
            "bsp_audio.c must reconfigure I2S std clock for 24 kHz playback", failures)
    require("BSP_AUDIO_SAMPLE_RATE == AUDIO_OPUS_SAMPLE_RATE" not in stream_source,
            "audio_opus_stream.c must not hard-fail playback because BSP default is 16 kHz", failures)

    open_body = function_body(bsp_source, "bsp_audio_open")
    require("s_codec_opened" in open_body and "return ESP_OK" in open_body,
            "bsp_audio_open must preserve an already-open codec sample rate", failures)
    reconfig_body = function_body(bsp_source, "reconfigure_i2s_sample_rate")
    require("old_sample_rate" in reconfig_body and "restore_i2s_sample_rate" in bsp_source,
            "reconfigure_i2s_sample_rate must restore the previous I2S rate after failures", failures)
    open_rate_body = function_body(bsp_source, "bsp_audio_open_with_sample_rate_locked")
    require("old_sample_rate" in open_rate_body and "old_fs" in open_rate_body,
            "bsp_audio_open_with_sample_rate must recover codec/I2S state after open failures", failures)
    require("s_audio_lock" in bsp_source and "xSemaphoreCreateMutex" in bsp_source,
            "bsp_audio.c must serialize audio open and volume operations with a mutex", failures)

    stream_start_body = function_body(stream_source, "audio_opus_stream_start")
    require("config->pcm_source == s_stream.pcm_source" in stream_start_body,
            "audio_opus_stream_start must reject pcm_source changes while running", failures)


def check_voice_session_task(failures: list[str]) -> None:
    controller = read("main/app/app_controller.c")
    stage1 = read("main/app/xiaozhi_stage1.c")
    app_start_body = function_body(controller, "app_controller_start")
    voice_body = function_body(controller, "voice_trigger_cb")

    require("voice_session_task" in controller,
            "app_controller.c must define a voice_session_task", failures)
    require("VOICE_SESSION_EVT_TEXT_TEST" in controller,
            "app_controller.c must name SW3 single click as VOICE_SESSION_EVT_TEXT_TEST", failures)
    require("你好，请介绍你自己" in controller,
            "app_controller.c must keep the P0 detect text literal visible", failures)
    require("voice_session_start()" not in app_start_body,
            "voice_session_task must not start before provisioning completes", failures)
    require("app_controller_start_voice_session()" in stage1,
            "voice_session_task must start from xiaozhi_stage1 via app_controller_start_voice_session", failures)
    require("xQueueSend" in voice_body,
            "voice_trigger_cb must enqueue a voice event", failures)
    require("xiaozhi_ws_trigger_listen" not in voice_body,
            "voice_trigger_cb must not call xiaozhi_ws_trigger_listen directly", failures)


def check_detect_text_request(failures: list[str]) -> None:
    ws_source = read("main/xiaozhi/xiaozhi_ws.c")
    body = function_body(ws_source, "xiaozhi_ws_trigger_detect_text")

    require("xiaozhi_protocol_build_listen_detect_json" in body,
            "xiaozhi_ws_trigger_detect_text must build listen detect JSON", failures)
    require("(void)text;" not in body,
            "xiaozhi_ws_trigger_detect_text must not ignore text", failures)
    require("ensure_websocket_ready()" in body,
            "detect text must wait for websocket READY before sending", failures)
    require("XIAOZHI_WS_STATE_WAITING_RESPONSE" in body,
            "detect text must transition to WAITING_RESPONSE after send", failures)


def main() -> int:
    failures: list[str] = []
    check_sdkconfig_defaults(failures)
    check_codec_split(failures)
    check_downlink_sample_rate(failures)
    check_stream_direct_capture(failures)
    check_detect_text_request(failures)
    check_voice_session_task(failures)

    if failures:
        print("P0 voice loop guardrails failed:")
        for failure in failures:
            print(f"- {failure}")
        return 1

    print("P0 voice loop guardrails passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
