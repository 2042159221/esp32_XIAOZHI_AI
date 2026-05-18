#!/usr/bin/env python3
"""Validate P2 multi-turn voice resilience from ESP-IDF monitor logs."""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path


TX_FRAMES_RE = re.compile(r"tx_frames=(\d+)")
TX_BYTES_RE = re.compile(r"tx_bytes=(\d+)")


@dataclass
class VoiceTurn:
    start_line: int
    listen_start: bool = False
    ready_to_listening: bool = False
    pcm_stats: bool = False
    opus_sent: bool = False
    stop_event: bool = False
    listen_stop_payload: bool = False
    listening_to_waiting: bool = False
    waiting_to_speaking: bool = False
    speaking_to_ready: bool = False
    waiting_timeout: bool = False
    waiting_timeout_ready: bool = False
    speaking_idle_timeout: bool = False
    tx_frames: int | None = None
    tx_bytes: int | None = None
    end_line: int | None = None

    def note(self, line_no: int, line: str) -> None:
        if "listen start mode=manual" in line:
            self.listen_start = True
        if "state transition READY -> LISTENING" in line:
            self.ready_to_listening = True
        if "direct capture pcm stats" in line:
            self.pcm_stats = True
        if "opus frame sent len=" in line:
            self.opus_sent = True
        if "SW3 manual listen stop event" in line:
            self.stop_event = True
        if "listen stop payload=" in line:
            self.listen_stop_payload = True
        if "state transition LISTENING -> WAITING_RESPONSE" in line:
            self.listening_to_waiting = True
        if "state transition WAITING_RESPONSE -> SPEAKING" in line:
            self.waiting_to_speaking = True
        if "state transition SPEAKING -> READY" in line:
            self.speaking_to_ready = True
            self.end_line = line_no
        if "WAITING_RESPONSE timeout after" in line:
            self.waiting_timeout = True
            self.tx_frames = first_int(TX_FRAMES_RE, line)
            self.tx_bytes = first_int(TX_BYTES_RE, line)
        if "state transition WAITING_RESPONSE -> READY" in line:
            self.waiting_timeout_ready = True
            self.end_line = line_no
        if "SPEAKING idle timeout" in line:
            self.speaking_idle_timeout = True

    def is_complete(self) -> bool:
        start_ok = self.listen_start and self.ready_to_listening
        uplink_ok = self.pcm_stats and self.opus_sent
        stop_ok = self.stop_event and self.listen_stop_payload and self.listening_to_waiting
        response_ok = (
            self.waiting_to_speaking and (self.speaking_to_ready or self.speaking_idle_timeout)
        ) or (self.waiting_timeout and self.waiting_timeout_ready)
        return start_ok and uplink_ok and stop_ok and response_ok


def first_int(pattern: re.Pattern[str], line: str) -> int | None:
    match = pattern.search(line)
    return int(match.group(1)) if match else None


def read_lines(log_path: str) -> list[str]:
    if log_path == "-":
        return sys.stdin.read().splitlines()
    return Path(log_path).read_text(encoding="utf-8", errors="ignore").splitlines()


def parse_turns(lines: list[str]) -> list[VoiceTurn]:
    turns: list[VoiceTurn] = []
    current: VoiceTurn | None = None

    for line_no, line in enumerate(lines, start=1):
        if "SW3 manual listen start event" in line:
            if current is not None:
                turns.append(current)
            current = VoiceTurn(start_line=line_no)

        if current is None:
            continue

        current.note(line_no, line)
        if current.is_complete():
            turns.append(current)
            current = None

    if current is not None:
        turns.append(current)

    return turns


def has_all(lines: list[str], markers: list[str]) -> tuple[bool, list[str]]:
    text = "\n".join(lines)
    missing = [marker for marker in markers if marker not in text]
    return not missing, missing


def validate_turns(turns: list[VoiceTurn], min_turns: int, max_turns: int | None) -> list[str]:
    failures: list[str] = []
    complete = [turn for turn in turns if turn.is_complete()]

    if len(complete) < min_turns:
        failures.append(f"complete manual turns too low: expected>={min_turns} actual={len(complete)} total_seen={len(turns)}")
    if max_turns is not None and len(complete) > max_turns:
        failures.append(f"complete manual turns too high: expected<={max_turns} actual={len(complete)}")

    for index, turn in enumerate(turns, start=1):
        if turn.is_complete():
            continue
        failures.append(
            "turn %d incomplete start_line=%d listen_start=%d ready_to_listening=%d pcm_stats=%d opus_sent=%d "
            "stop_event=%d listen_stop_payload=%d listening_to_waiting=%d waiting_to_speaking=%d "
            "speaking_to_ready=%d waiting_timeout=%d waiting_timeout_ready=%d speaking_idle_timeout=%d"
            % (
                index,
                turn.start_line,
                turn.listen_start,
                turn.ready_to_listening,
                turn.pcm_stats,
                turn.opus_sent,
                turn.stop_event,
                turn.listen_stop_payload,
                turn.listening_to_waiting,
                turn.waiting_to_speaking,
                turn.speaking_to_ready,
                turn.waiting_timeout,
                turn.waiting_timeout_ready,
                turn.speaking_idle_timeout,
            )
        )

    return failures


def validate_required_sequences(args: argparse.Namespace, lines: list[str]) -> list[str]:
    failures: list[str] = []

    if args.require_waiting_timeout:
        ok, missing = has_all(lines, [
            "WAITING_RESPONSE timeout after",
            "state transition WAITING_RESPONSE -> READY",
        ])
        if not ok:
            failures.append("missing WAITING_RESPONSE timeout recovery markers: " + ", ".join(missing))

    if args.require_speaking_timeout:
        ok, missing = has_all(lines, [
            "SPEAKING idle timeout",
            "state transition SPEAKING -> READY",
        ])
        if not ok:
            failures.append("missing SPEAKING idle recovery markers: " + ", ".join(missing))

    if args.require_disconnect_recovery:
        ok, missing = has_all(lines, [
            "websocket disconnected",
            "cleanup stale websocket client before reconnect",
            "state transition DISCONNECTED -> CONNECTING",
            "state transition CONNECTING -> WS_CONNECTED",
            "state transition WS_CONNECTED -> HELLO_SENT",
            "state transition HELLO_SENT -> READY",
        ])
        if not ok:
            failures.append("missing WebSocket disconnect recovery markers: " + ", ".join(missing))

    return failures


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", required=True, help="ESP-IDF monitor log path, or '-' for stdin")
    parser.add_argument("--min-turns", type=int, default=10, help="minimum complete manual voice turns")
    parser.add_argument("--max-turns", type=int, default=None, help="optional maximum complete manual voice turns")
    parser.add_argument("--require-waiting-timeout", action="store_true", help="require WAITING_RESPONSE timeout recovery markers")
    parser.add_argument("--require-speaking-timeout", action="store_true", help="require SPEAKING idle watchdog recovery markers")
    parser.add_argument("--require-disconnect-recovery", action="store_true", help="require WebSocket disconnect and reconnect markers")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    if args.min_turns < 0:
        print("--min-turns must be >= 0", file=sys.stderr)
        return 2
    if args.max_turns is not None and args.max_turns < args.min_turns:
        print("--max-turns must be >= --min-turns", file=sys.stderr)
        return 2

    lines = read_lines(args.log)
    turns = parse_turns(lines)
    failures = validate_turns(turns, args.min_turns, args.max_turns)
    failures.extend(validate_required_sequences(args, lines))

    complete_turns = sum(1 for turn in turns if turn.is_complete())
    if failures:
        print("P2 voice resilience log validation failed:")
        for failure in failures:
            print(f"- {failure}")
        print(f"summary: complete_turns={complete_turns} total_turns={len(turns)}")
        return 1

    print(f"P2 voice resilience log validation passed: complete_turns={complete_turns} total_turns={len(turns)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
