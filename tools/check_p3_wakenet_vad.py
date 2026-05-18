#!/usr/bin/env python3
"""Static P3 WakeNet/VAD automatic dialogue guardrails."""

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


def main() -> int:
    failures: list[str] = []
    defaults = read("sdkconfig.defaults")
    sdkconfig = read("sdkconfig")
    kconfig = read("main/Kconfig.projbuild")
    stage1 = read("main/app/xiaozhi_stage1.c")
    sr_header = read("main/services/sr/xiaozhi_sr.h")
    sr_source = read("main/services/sr/xiaozhi_sr.c")
    ws_source = read("main/xiaozhi/xiaozhi_ws.c")
    stream_header = read("main/services/audio/audio_opus_stream.h")
    stream_source = read("main/services/audio/audio_opus_stream.c")

    handle_hello = function_body(ws_source, "handle_server_hello")
    handle_binary = function_body(ws_source, "handle_binary_opus")
    handle_tts = function_body(ws_source, "handle_tts")
    vad_body = function_body(ws_source, "xiaozhi_ws_on_vad_state")
    feed_body = function_body(ws_source, "xiaozhi_ws_feed_processed_pcm")
    manual_start = function_body(ws_source, "start_manual_listen_now")
    stop_body = function_body(ws_source, "xiaozhi_ws_stop_listen")
    stream_start = function_body(stream_source, "audio_opus_stream_start")

    require("CONFIG_XIAOZHI_STAGE1_AUTO_SR_ENABLE=y" in defaults,
            "P3 defaults must enable automatic SR", failures)
    require("CONFIG_XIAOZHI_STAGE1_AUTO_SR_ENABLE=y" in sdkconfig,
            "P3 sdkconfig must enable automatic SR", failures)
    require("# CONFIG_XIAOZHI_STAGE1_WAKE_ONLY_SR_ENABLE is not set" in defaults,
            "P3 defaults must keep wake-only experiment disabled", failures)
    require("# CONFIG_XIAOZHI_STAGE1_WAKE_ONLY_SR_ENABLE is not set" in sdkconfig,
            "P3 sdkconfig must keep wake-only experiment disabled", failures)
    require("Start WakeNet/VAD SR" in kconfig or "automatic dialogue" in kconfig,
            "Kconfig help must describe full P3 automatic dialogue semantics", failures)

    require("xiaozhi_sr_pause" in sr_header and "xiaozhi_sr_resume" in sr_header,
            "SR service must expose pause/resume ownership controls", failures)
    require("s_paused" in sr_source,
            "SR service must track paused state to stop feeding AFE while another owner uses codec", failures)
    require("s_paused" in function_body(sr_source, "sr_feed_task"),
            "SR feed task must honor pause state before esp_codec_dev_read", failures)

    require("AUDIO_OPUS_STREAM_FLAG_SKIP_AUDIO_PATH_OPEN" in stream_header,
            "audio stream config must support skipping codec open for SR external PCM uplink", failures)
    require("flags" in stream_header and "flags" in stream_start,
            "audio stream start must consume config flags", failures)
    require("AUDIO_OPUS_STREAM_FLAG_UPLINK_ONLY" in stream_header,
            "audio stream config must support uplink-only mode without decoder/downlink task", failures)
    require("skip_audio_path_open" in stream_source,
            "audio stream implementation must skip codec open when requested", failures)
    require("uplink_only" in stream_source,
            "audio stream implementation must skip decoder task in uplink-only mode", failures)

    require("CONFIG_XIAOZHI_STAGE1_AUTO_SR_ENABLE" in handle_hello,
            "server hello handling must branch for automatic SR mode", failures)
    require("start_audio_stream(AUDIO_OPUS_PCM_SOURCE_EXTERNAL_FEED)" not in handle_hello,
            "P3 server hello must not unconditionally start downlink stream and steal SR codec", failures)
    require("set_state(XIAOZHI_WS_STATE_READY)" in handle_hello,
            "server hello must still enter READY", failures)

    require("start_sr_uplink_stream" in vad_body,
            "VAD speech must start SR external PCM uplink stream", failures)
    require('send_listen_state("start", "auto")' in vad_body,
            "VAD speech must send listen start with mode=auto", failures)
    require("AUDIO_OPUS_PCM_SOURCE_EXTERNAL_FEED" in vad_body,
            "VAD speech must use SR external PCM feed, not direct codec capture", failures)
    require("xiaozhi_ws_stop_listen()" in vad_body,
            "VAD silence must reuse listen stop response handling", failures)
    require("audio_opus_stream_feed_pcm" in feed_body,
            "SR processed PCM must feed Opus uplink while LISTENING", failures)

    require("xiaozhi_sr_pause" in handle_binary or "xiaozhi_sr_stop" in handle_binary,
            "binary TTS playback must release SR codec ownership before downlink playback", failures)
    require("ensure_downlink_audio_stream" in handle_binary,
            "binary TTS playback must lazily start downlink audio stream", failures)
    require("ensure_downlink_audio_stream" in handle_tts,
            "TTS start must prepare downlink stream only when playback is actually needed", failures)
    require("xiaozhi_sr_resume" in handle_tts,
            "TTS stop must resume SR after playback completes", failures)

    require("CONFIG_XIAOZHI_STAGE1_AUTO_SR_ENABLE" in manual_start,
            "manual PTT must branch in automatic SR mode", failures)
    require("AUDIO_OPUS_PCM_SOURCE_DIRECT_CODEC" in manual_start,
            "manual PTT must retain direct codec path when SR auto mode is disabled", failures)
    require("start_sr_uplink_stream" in manual_start,
            "manual PTT must use SR external PCM path when SR auto mode owns the mic", failures)
    require("CONFIG_XIAOZHI_STAGE1_AUTO_SR_ENABLE" in stop_body and "xiaozhi_sr_resume" in stop_body,
            "listen stop must restore SR ownership after non-speaking response paths", failures)

    if failures:
        print("P3 WakeNet/VAD guardrails failed:")
        for failure in failures:
            print(f"- {failure}")
        return 1

    print("P3 WakeNet/VAD guardrails passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
