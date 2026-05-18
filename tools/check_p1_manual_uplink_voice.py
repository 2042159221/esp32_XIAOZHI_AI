#!/usr/bin/env python3
"""Static P1 manual uplink voice guardrails for the ESP-IDF app."""

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


def require_before(body: str, first: str, second: str, message: str, failures: list[str]) -> None:
    first_index = body.find(first)
    second_index = body.find(second)
    require(first_index >= 0 and second_index >= 0 and first_index < second_index, message, failures)


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


def check_sw3_long_press_events(failures: list[str]) -> None:
    input_header = read("main/app/app_input_controller.h")
    input_source = read("main/app/app_input_controller.c")
    controller = read("main/app/app_controller.c")
    adc_body = function_body(input_source, "adc_button_cb")
    voice_cb_body = function_body(controller, "voice_event_cb")
    voice_task_body = function_body(controller, "voice_session_task")

    for symbol in (
        "APP_INPUT_VOICE_EVT_TEXT_TEST",
        "APP_INPUT_VOICE_EVT_LISTEN_START",
        "APP_INPUT_VOICE_EVT_LISTEN_STOP",
    ):
        require(symbol in input_header, f"app_input_controller.h must expose {symbol}", failures)

    require("app_input_voice_event_cb_t" in input_header,
            "app_input_controller.h must expose a voice event callback with the event id", failures)
    require("voice_event_cb" in controller,
            "app_controller.c must use a voice_event_cb that receives queued input events", failures)
    require("xQueueSend" in voice_cb_body and "evt" in voice_cb_body,
            "voice_event_cb must enqueue the received voice event", failures)
    require("BUTTON_SINGLE_CLICK" in adc_body and "APP_INPUT_VOICE_EVT_TEXT_TEST" in adc_body,
            "SW3 single click must still enqueue the P0 text test event", failures)
    require("BUTTON_LONG_PRESS_START" in adc_body and "APP_INPUT_VOICE_EVT_LISTEN_START" in adc_body,
            "SW3 long press start must enqueue listen start", failures)
    require("BUTTON_LONG_PRESS_UP" in adc_body and "APP_INPUT_VOICE_EVT_LISTEN_STOP" in adc_body,
            "SW3 long press release must enqueue listen stop", failures)
    require("VOICE_SESSION_EVT_LISTEN_START" in controller,
            "app_controller.c must define VOICE_SESSION_EVT_LISTEN_START", failures)
    require("VOICE_SESSION_EVT_LISTEN_STOP" in controller,
            "app_controller.c must define VOICE_SESSION_EVT_LISTEN_STOP", failures)
    require("xiaozhi_ws_trigger_listen(XIAOZHI_WS_LISTEN_MODE_BUTTON)" in voice_task_body,
            "voice_session_task must start manual button listening from the queue", failures)
    require("xiaozhi_ws_stop_listen()" in voice_task_body,
            "voice_session_task must stop manual button listening from the queue", failures)


def check_manual_listen_state_machine(failures: list[str]) -> None:
    ws_source = read("main/xiaozhi/xiaozhi_ws.c")
    trigger_body = function_body(ws_source, "xiaozhi_ws_trigger_listen")
    start_body = function_body(ws_source, "start_manual_listen_now")
    stop_body = function_body(ws_source, "xiaozhi_ws_stop_listen")
    hello_body = function_body(ws_source, "handle_server_hello")
    start_audio_body = function_body(ws_source, "start_audio_stream")

    require("return xiaozhi_ws_on_wake_detected()" not in trigger_body,
            "xiaozhi_ws_trigger_listen must not delegate manual button start to wake detection", failures)
    require("XIAOZHI_WS_LISTEN_MODE_BUTTON" in trigger_body,
            "xiaozhi_ws_trigger_listen must handle button listen mode explicitly", failures)
    require('send_listen_state("start", "manual")' in start_body,
            "manual listen start must send state=start mode=manual", failures)
    require("AUDIO_OPUS_PCM_SOURCE_DIRECT_CODEC" in start_body,
            "manual listen start must use direct codec capture", failures)
    require("AUDIO_OPUS_SAMPLE_RATE" in start_body and "bsp_audio_open_with_sample_rate" in ws_source,
            "manual listen start must switch the audio path to the 16 kHz Opus uplink rate", failures)
    require("audio_opus_stream_wait_downlink_idle" in start_body,
            "manual listen start must drain downlink before switching the shared audio path", failures)
    require("XIAOZHI_WS_STATE_SPEAKING" in trigger_body and "s_waiting_tts_stop" in trigger_body,
            "manual listen start must ignore requests while TTS is speaking or waiting for TTS stop", failures)
    require("XIAOZHI_WS_STATE_WAITING_RESPONSE" in trigger_body,
            "manual listen start must not create stale pending PTT while waiting for response", failures)
    require("XIAOZHI_WS_STATE_LISTENING" in start_body,
            "manual listen start must enter LISTENING after enabling uplink", failures)
    require("audio_opus_stream_set_uplink_enabled(true)" in start_body,
            "manual listen start must enable the Opus uplink", failures)
    require_before(start_body,
                   "set_state(XIAOZHI_WS_STATE_LISTENING)",
                   "audio_opus_stream_set_uplink_enabled(true)",
                   "manual listen start must enter LISTENING before enabling uplink to avoid dropping early Opus frames",
                   failures)
    require("s_pending_ptt" in ws_source and "s_button_down" in ws_source,
            "manual listen must track pending_ptt and button_down separately", failures)
    require("s_button_down = true" in trigger_body and "s_pending_ptt = true" in trigger_body,
            "SW3 long press before READY must remember button_down and pending_ptt", failures)
    require("wait_for_ready(" not in trigger_body and "ensure_websocket_ready()" not in trigger_body,
            "manual listen start must not block waiting for WebSocket READY", failures)
    require("start_pending_ptt_if_ready" in hello_body,
            "WebSocket READY callback must continue a pending PTT only after server hello", failures)
    require("s_button_down = false" in stop_body and "s_pending_ptt = false" in stop_body,
            "SW3 release before LISTENING must cancel pending PTT without sending listen stop", failures)

    require("audio_opus_stream_set_uplink_enabled(false)" in stop_body,
            "listen stop must disable uplink immediately", failures)
    require("audio_opus_stream_flush" in stop_body,
            "listen stop must flush pending uplink PCM before the next turn", failures)
    require("xiaozhi_protocol_build_listen_stop_json" in stop_body,
            "listen stop must send the protocol listen stop payload", failures)
    require("AUDIO_OPUS_PCM_SOURCE_EXTERNAL_FEED" in stop_body,
            "listen stop must restore the downlink playback stream after direct capture", failures)
    require("set_waiting_response" in stop_body,
            "listen stop must enter WAITING_RESPONSE through timeout recovery when the stop payload is sent", failures)
    require("decoder_output_sample_rate = resolve_decoder_output_sample_rate()" in start_audio_body,
            "downlink decoder sample rate must continue to follow server hello", failures)


def check_waiting_response_recovery(failures: list[str]) -> None:
    ws_header = read("main/xiaozhi/xiaozhi_ws.h")
    ws_source = read("main/xiaozhi/xiaozhi_ws.c")
    stream_header = read("main/services/audio/audio_opus_stream.h")
    stream_source = read("main/services/audio/audio_opus_stream.c")
    stop_body = function_body(ws_source, "xiaozhi_ws_stop_listen")
    detect_body = function_body(ws_source, "xiaozhi_ws_trigger_detect_text")
    timeout_body = function_body(ws_source, "waiting_response_timeout_cb")

    require("XIAOZHI_WS_RESPONSE_TIMEOUT_MS" in ws_source and "XIAOZHI_WS_SHORT_RESPONSE_TIMEOUT_MS" in ws_source,
            "WAITING_RESPONSE must have normal and short recovery timeouts", failures)
    require("xTimerCreate" in ws_source and "waiting_response_timeout_cb" in ws_source,
            "WAITING_RESPONSE recovery must use a FreeRTOS one-shot timer", failures)
    require("set_waiting_response" in stop_body and "set_waiting_response" in detect_body,
            "listen stop and P0 detect text must enter WAITING_RESPONSE through the timeout helper", failures)
    require("audio_opus_stream_get_stats" in stop_body,
            "listen stop must inspect uplink tx stats before choosing the response timeout", failures)
    require("XIAOZHI_WS_MIN_LISTEN_TX_FRAMES" in stop_body and "XIAOZHI_WS_MIN_LISTEN_MS" in stop_body,
            "listen stop must apply the minimum recording protection", failures)
    require("tx_frames" in timeout_body and "tx_bytes" in timeout_body and "last_state" in timeout_body,
            "WAITING_RESPONSE timeout log must include tx_frames, tx_bytes, and last_state", failures)
    require("set_state(XIAOZHI_WS_STATE_READY)" in timeout_body,
            "WAITING_RESPONSE timeout must recover to READY", failures)
    require("note_waiting_response_activity" in ws_source,
            "server response activity must refresh or cancel WAITING_RESPONSE timeout", failures)
    activity_body = function_body(ws_source, "note_waiting_response_activity")
    require("s_waiting_response_timeout_ms = XIAOZHI_WS_RESPONSE_TIMEOUT_MS" in activity_body,
            "WAITING_RESPONSE activity refresh must update the logged timeout value", failures)
    require("audio_opus_stream_stats_t" in stream_header and "audio_opus_stream_get_stats" in stream_header,
            "audio_opus_stream must expose tx stats for state-machine recovery diagnostics", failures)
    require("audio_opus_stream_get_stats" in stream_source and "tx_bytes" in stream_source,
            "audio_opus_stream.c must implement tx stats access", failures)
    require("xiaozhi_ws_request_ready" in ws_header and "wait_for_ready(" not in detect_body,
            "P0 detect text request must not block app_controller waiting for WebSocket READY", failures)


def check_p2_resilience_guardrails(failures: list[str]) -> None:
    ws_source = read("main/xiaozhi/xiaozhi_ws.c")
    stream_source = read("main/services/audio/audio_opus_stream.c")
    tts_body = function_body(ws_source, "handle_tts")
    binary_body = function_body(ws_source, "handle_binary_opus")
    speaking_timeout_body = function_body(ws_source, "speaking_timeout_cb")
    stop_body = function_body(ws_source, "xiaozhi_ws_stop_listen")
    watermarks_body = function_body(stream_source, "audio_opus_stream_log_watermarks")
    flush_body = function_body(stream_source, "audio_opus_stream_flush")

    require("XIAOZHI_WS_SPEAKING_IDLE_TIMEOUT_MS" in ws_source,
            "P2 speaking playback must define an idle watchdog timeout", failures)
    require("s_speaking_timeout_timer" in ws_source and "speaking_timeout_cb" in ws_source,
            "P2 speaking playback must use a FreeRTOS one-shot watchdog timer", failures)
    require("note_speaking_activity" in tts_body and "note_speaking_activity" in binary_body,
            "P2 TTS start and binary downlink must refresh the speaking watchdog", failures)
    require("cancel_speaking_timeout_timer" in tts_body and 'strcmp(msg->state, "stop") == 0' in tts_body,
            "P2 TTS stop must cancel the speaking watchdog", failures)
    require("set_state(XIAOZHI_WS_STATE_READY)" in speaking_timeout_body and "s_waiting_tts_stop = false" in speaking_timeout_body,
            "P2 speaking watchdog timeout must recover to READY and clear TTS wait state", failures)
    require("cancel_speaking_timeout_timer" in ws_source and "reset_session_flags" in ws_source,
            "P2 session reset must cancel the speaking watchdog", failures)

    require("s_manual_listen_start_tick = 0" in stop_body,
            "P2 listen stop must clear manual listen start tick after computing the turn duration", failures)
    require("s_stream.downlink_pending_frames = 0" in flush_body,
            "P2 audio stream flush must reset stale downlink pending frame counters", failures)
    require("pending_downlink_reset_when_stopped" in stream_source,
            "P2 audio stream must only force-reset downlink pending counters after stopping the stream", failures)
    require("s_stream.downlink_pending_frames = 0" not in flush_body,
            "P2 audio stream flush must not clear pending downlink counters while decoder may still be active", failures)

    for field in (
        "pcm_rb_free",
        "pcm_rb_max_item",
        "downlink_rb_free",
        "downlink_rb_max_item",
        "minimum_free_heap",
    ):
        require(field in watermarks_body,
                f"P2 audio stream watermarks must include {field}", failures)


def check_p2_log_resilience_tool(failures: list[str]) -> None:
    tool_path = ROOT / "tools/check_p2_voice_resilience.py"
    require(tool_path.exists(),
            "P2 must provide a runtime log validator at tools/check_p2_voice_resilience.py", failures)
    if not tool_path.exists():
        return

    tool_source = tool_path.read_text(encoding="utf-8")
    for symbol in (
        "--min-turns",
        "--require-waiting-timeout",
        "--require-disconnect-recovery",
        "parse_turns",
        "WAITING_RESPONSE timeout after",
        "SPEAKING idle timeout",
        "cleanup stale websocket client before reconnect",
        "state transition SPEAKING -> READY",
    ):
        require(symbol in tool_source,
                f"P2 runtime log validator must check {symbol}", failures)
    require("self.waiting_to_speaking and self.speaking_to_ready" in tool_source,
            "P2 runtime log validator must require SPEAKING -> READY before counting a speaking turn complete", failures)
    require("or self.speaking_idle_timeout" not in tool_source,
            "P2 runtime log validator must not treat SPEAKING idle timeout alone as a complete turn", failures)


def check_pcm_diagnostics_and_sample_contract(failures: list[str]) -> None:
    codec_header = read("main/services/audio/audio_opus_codec.h")
    codec_source = read("main/services/audio/audio_opus_codec.c")
    stream_source = read("main/services/audio/audio_opus_stream.c")
    direct_body = function_body(stream_source, "direct_capture_task")

    require("#define AUDIO_OPUS_SAMPLE_RATE 16000" in codec_header,
            "uplink Opus encoder sample rate must remain fixed at 16 kHz", failures)
    require("#define AUDIO_OPUS_FRAME_DURATION_MS 60" in codec_header,
            "uplink Opus frame duration must remain 60 ms by default", failures)
    require("enc_cfg.sample_rate = AUDIO_OPUS_SAMPLE_RATE" in codec_source,
            "Opus encoder must use AUDIO_OPUS_SAMPLE_RATE", failures)
    require("AUDIO_OPUS_PCM_FRAME_BYTES" in codec_header,
            "Opus PCM frame byte contract must stay centralized", failures)

    for symbol in (
        "log_direct_capture_pcm_stats",
        "pcm_min",
        "pcm_max",
        "pcm_rms",
        "zero_samples",
        "clip_samples",
    ):
        require(symbol in stream_source,
                f"audio_opus_stream.c must include direct capture PCM diagnostic field {symbol}", failures)
    require("log_direct_capture_pcm_stats(pcm_frame, s_stream.pcm_frame_bytes)" in direct_body,
            "direct capture must log PCM statistics for captured microphone frames", failures)


def check_p1_quality_fixes(failures: list[str]) -> None:
    input_source = read("main/app/app_input_controller.c")
    stream_source = read("main/services/audio/audio_opus_stream.c")
    bsp_source = read("main/bsp/audio/bsp_audio.c")
    kconfig = read("main/Kconfig.projbuild")
    stream_start_body = function_body(stream_source, "audio_opus_stream_start")
    uplink_body = function_body(stream_source, "audio_opus_stream_set_uplink_enabled")
    register_body = function_body(input_source, "register_adc_button_events")

    require("BUTTON_LONG_PRESS_HOLD" not in register_body,
            "SW3 long press hold must not be registered because it floods logs during realtime capture", failures)
    require("ensure_encoder_locked()" not in stream_start_body,
            "audio_opus_stream_start must not pre-open the Opus encoder for downlink-only playback", failures)
    require("ensure_encoder_locked()" in uplink_body,
            "audio_opus_stream_set_uplink_enabled(true) must open the Opus encoder only when uplink starts", failures)

    require("s_i2s_tx_enabled" in bsp_source and "s_i2s_rx_enabled" in bsp_source,
            "bsp_audio.c must track I2S TX/RX enabled state before disabling channels", failures)
    require("disable_i2s_channel_if_enabled" in bsp_source,
            "bsp_audio.c must wrap I2S disable to avoid invalid-state driver errors", failures)
    require(bsp_source.count("i2s_channel_disable(") == 1,
            "bsp_audio.c must call i2s_channel_disable only inside the state-aware wrapper", failures)

    require("config XIAOZHI_AUDIO_MIC_GAIN_DB" in kconfig,
            "Kconfig must expose XIAOZHI_AUDIO_MIC_GAIN_DB for ES8311 microphone gain tuning", failures)
    require("esp_codec_dev_set_in_gain" in bsp_source and "CONFIG_XIAOZHI_AUDIO_MIC_GAIN_DB" in bsp_source,
            "bsp_audio.c must apply configured ES8311 microphone gain after codec open", failures)


def check_sr_auto_init_disabled(failures: list[str]) -> None:
    defaults = read("sdkconfig.defaults")
    sdkconfig = read("sdkconfig")
    stage1 = read("main/app/xiaozhi_stage1.c")

    require("# CONFIG_XIAOZHI_STAGE1_AUTO_SR_ENABLE is not set" in defaults,
            "sdkconfig.defaults must keep SR auto init disabled for P1", failures)
    require("# CONFIG_XIAOZHI_STAGE1_AUTO_SR_ENABLE is not set" in sdkconfig,
            "sdkconfig must keep SR auto init disabled for P1", failures)
    require("#if CONFIG_XIAOZHI_STAGE1_AUTO_SR_ENABLE" in stage1,
            "stage1 SR startup must remain gated by CONFIG_XIAOZHI_STAGE1_AUTO_SR_ENABLE", failures)


def main() -> int:
    failures: list[str] = []
    check_sw3_long_press_events(failures)
    check_manual_listen_state_machine(failures)
    check_waiting_response_recovery(failures)
    check_p2_resilience_guardrails(failures)
    check_p2_log_resilience_tool(failures)
    check_pcm_diagnostics_and_sample_contract(failures)
    check_p1_quality_fixes(failures)
    check_sr_auto_init_disabled(failures)

    if failures:
        print("P1 manual uplink voice guardrails failed:")
        for failure in failures:
            print(f"- {failure}")
        return 1

    print("P1 manual uplink voice guardrails passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
