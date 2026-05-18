#!/usr/bin/env python3
"""Guardrails for moving Xiaozhi timeout work out of FreeRTOS timer callbacks."""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(rel_path: str) -> str:
    return (ROOT / rel_path).read_text(encoding="utf-8", errors="ignore")


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
    ws = read("main/xiaozhi/xiaozhi_ws.c")
    sdkconfig = read("sdkconfig")
    defaults = read("sdkconfig.defaults")

    wait_cb = function_body(ws, "waiting_response_timeout_cb")
    session_task = function_body(ws, "xiaozhi_ws_session_task")
    wait_handler = function_body(ws, "handle_waiting_response_timeout_event")
    stop_body = function_body(ws, "xiaozhi_ws_stop_listen")
    vad_body = function_body(ws, "xiaozhi_ws_on_vad_state")

    require("xiaozhi_ws_post_session_event" in wait_cb,
            "WAITING_RESPONSE timer callback must only post a lightweight session event", failures)
    forbidden_wait_cb = (
        "set_state(",
        "audio_opus_stream_stop",
        "stop_opus_audio_stream",
        "xiaozhi_sr_resume",
        "xiaozhi_sr_pause",
        "esp_websocket_client_send",
        "esp_websocket_client_close",
        "cJSON_",
        "malloc",
        "free(",
        "heap_caps_get_",
        "log_heap_stats",
    )
    for token in forbidden_wait_cb:
        require(token not in wait_cb, f"WAITING_RESPONSE timer callback must not call {token}", failures)

    require("XIAOZHI_WS_EVT_WAIT_RESPONSE_TIMEOUT" in ws,
            "xiaozhi_ws.c must define a WAIT_RESPONSE_TIMEOUT session event", failures)
    require("xQueueCreate" in ws and "xQueueSend" in ws and "xQueueReceive" in ws,
            "xiaozhi_ws.c must use a queue-backed session task for timeout work", failures)
    require("xTaskGetCurrentTaskHandle" in wait_cb or "pcTaskGetName(NULL)" in wait_cb,
            "timer callback must log the current task name for diagnostics", failures)
    require("XIAOZHI_WS_EVT_WAIT_RESPONSE_TIMEOUT" in session_task,
            "session task must receive WAIT_RESPONSE_TIMEOUT events", failures)
    require("WAITING_RESPONSE timeout event" in wait_handler,
            "timeout handler must log timeout event handling in task context", failures)
    require("pcTaskGetName(NULL)" in wait_handler,
            "timeout handler must log current task name to prove it is not Tmr Svc", failures)
    require("set_state(XIAOZHI_WS_STATE_READY)" in wait_handler,
            "WAIT_RESPONSE_TIMEOUT event handler must recover WAITING_RESPONSE to READY", failures)

    require("listen_start_time_us" in ws and "esp_timer_get_time" in ws,
            "listen duration must use initialized microsecond timestamps", failures)
    require("XIAOZHI_WS_LISTEN_MODE_AUTO" in ws and "XIAOZHI_WS_LISTEN_MODE_MANUAL" in ws,
            "auto/manual listen modes must be tracked separately", failures)
    require("mode=%s listen_ms=%u tx_frames=%u tx_bytes=%u silence_ms=%u" in stop_body,
            "listen stop log must include mode, listen_ms, tx_frames, tx_bytes, and silence_ms", failures)
    require("manual listen too short" not in stop_body,
            "auto VAD path must not print manual listen too short", failures)
    require("XIAOZHI_WS_AUTO_SILENCE_STOP_MS" in ws and "XIAOZHI_WS_AUTO_MAX_LISTEN_MS" in ws,
            "automatic endpointing must define silence stop and max listen windows", failures)
    require("XIAOZHI_WS_EVT_AUTO_SILENCE_TIMEOUT" in ws and "XIAOZHI_WS_EVT_AUTO_MAX_LISTEN_TIMEOUT" in ws,
            "automatic endpointing timers must post session events", failures)
    require("state=%s" in vad_body and "SPEAKING" in vad_body,
            "VAD speech must explicitly log/ignore playback-muted speaking state", failures)

    require("CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH=4096" in sdkconfig or
            "CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH=6144" in sdkconfig,
            "sdkconfig must raise FreeRTOS timer task stack depth as a safety margin", failures)
    require("CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH=4096" in defaults or
            "CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH=6144" in defaults,
            "sdkconfig.defaults must document raised timer task stack depth", failures)

    if failures:
        print("Timer service stack guardrails failed:")
        for failure in failures:
            print(f"- {failure}")
        return 1

    print("Timer service stack guardrails passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
