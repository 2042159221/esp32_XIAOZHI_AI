#!/usr/bin/env python3
"""WebSocket binary PCM echo server for ESP32 audio diagnostics."""

from __future__ import annotations

import argparse
import asyncio
import logging
import signal
import sys
import time
from dataclasses import dataclass

try:
    import websockets
except ImportError as exc:  # pragma: no cover - user environment guard
    print("Missing dependency: websockets", file=sys.stderr)
    print("Install with: python -m pip install websockets", file=sys.stderr)
    raise SystemExit(2) from exc


LOG = logging.getLogger("ws_audio_echo")


@dataclass
class ClientStats:
    peer: str
    started_at: float
    frames: int = 0
    bytes_in: int = 0
    bytes_out: int = 0
    text_frames: int = 0


def configure_logging(verbose: bool) -> None:
    logging.basicConfig(
        level=logging.DEBUG if verbose else logging.INFO,
        format="%(asctime)s.%(msecs)03d %(levelname)s %(name)s: %(message)s",
        datefmt="%H:%M:%S",
    )


async def log_periodic_stats(stats: ClientStats, stop_event: asyncio.Event, interval_s: float) -> None:
    last_frames = 0
    last_bytes_in = 0
    last_t = time.monotonic()

    while not stop_event.is_set():
        try:
            await asyncio.wait_for(stop_event.wait(), timeout=interval_s)
        except asyncio.TimeoutError:
            pass

        now = time.monotonic()
        dt = max(now - last_t, 0.001)
        frame_delta = stats.frames - last_frames
        byte_delta = stats.bytes_in - last_bytes_in
        LOG.info(
            "client=%s frames=%d bytes_in=%d bytes_out=%d fps=%.1f kbps_in=%.1f text_frames=%d",
            stats.peer,
            stats.frames,
            stats.bytes_in,
            stats.bytes_out,
            frame_delta / dt,
            (byte_delta * 8 / 1000) / dt,
            stats.text_frames,
        )
        last_frames = stats.frames
        last_bytes_in = stats.bytes_in
        last_t = now


def websocket_path(websocket: object, fallback_path: str | None) -> str:
    if fallback_path:
        return fallback_path

    request = getattr(websocket, "request", None)
    request_path = getattr(request, "path", None)
    if request_path:
        return request_path

    return getattr(websocket, "path", "<unknown>")


async def echo_handler(websocket: object, stats_interval_s: float, path: str | None = None) -> None:
    peer = f"{websocket.remote_address[0]}:{websocket.remote_address[1]}" if websocket.remote_address else "<unknown>"
    stats = ClientStats(peer=peer, started_at=time.monotonic())
    stop_event = asyncio.Event()
    stats_task = asyncio.create_task(log_periodic_stats(stats, stop_event, stats_interval_s))

    LOG.info("client connected: %s path=%s", peer, websocket_path(websocket, path))
    try:
        async for message in websocket:
            if isinstance(message, bytes):
                stats.frames += 1
                stats.bytes_in += len(message)
                await websocket.send(message)
                stats.bytes_out += len(message)
                if stats.frames <= 5 or stats.frames % 50 == 0:
                    LOG.info("binary frame client=%s count=%d len=%d", peer, stats.frames, len(message))
            else:
                stats.text_frames += 1
                LOG.info("text frame client=%s len=%d body=%r", peer, len(message), message[:120])
                await websocket.send(message)
    except websockets.ConnectionClosed as exc:
        LOG.info("client disconnected: %s code=%s reason=%s", peer, exc.code, exc.reason)
    finally:
        stop_event.set()
        await stats_task
        elapsed = max(time.monotonic() - stats.started_at, 0.001)
        LOG.info(
            "client summary: %s elapsed=%.2fs frames=%d bytes_in=%d bytes_out=%d avg_kbps_in=%.1f",
            peer,
            elapsed,
            stats.frames,
            stats.bytes_in,
            stats.bytes_out,
            (stats.bytes_in * 8 / 1000) / elapsed,
        )


async def run_server(host: str, port: int, stats_interval_s: float) -> None:
    stop = asyncio.Future()
    loop = asyncio.get_running_loop()

    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, stop.set_result, None)
        except NotImplementedError:
            pass

    async def handler(websocket: object, path: str | None = None) -> None:
        await echo_handler(websocket, stats_interval_s, path)

    async with websockets.serve(handler, host, port, max_size=None, compression=None):
        LOG.info("WebSocket PCM echo server listening on ws://%s:%d", host, port)
        LOG.info("Expected audio format: raw PCM, 16 kHz, 16-bit signed little-endian, mono")
        await stop


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="0.0.0.0", help="listen address")
    parser.add_argument("--port", type=int, default=8765, help="listen port")
    parser.add_argument("--stats-interval", type=float, default=1.0, help="per-client stats interval in seconds")
    parser.add_argument("-v", "--verbose", action="store_true", help="enable debug logging")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    configure_logging(args.verbose)
    try:
        asyncio.run(run_server(args.host, args.port, args.stats_interval))
    except KeyboardInterrupt:
        LOG.info("server stopped by keyboard interrupt")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
