# Xiaozhi P1 Manual Uplink Voice Design

## Goal

Build the first real voice conversation loop:

SW3 long press starts manual listening, ES8311/I2S RX captures microphone PCM, 16 kHz 60 ms PCM frames are encoded as OPUS, binary OPUS frames are sent through the existing authenticated WebSocket, SW3 release sends `listen stop`, and the server returns STT/TTS that continues through the already verified downlink playback path.

This is not an SR/VAD/AEC phase. P1 is a manual button voice uplink closure and stability baseline.

## Configuration

| Configuration item | Selection |
| --- | --- |
| Subagents | Disabled for design. Implementation can stay inline unless the user asks for parallel work. |
| Programming paradigm | Event-driven state machine with FreeRTOS task queues |
| Language | C for firmware, Python for static guardrails |
| Project type | ESP-IDF embedded voice application |
| Comment style | Minimal, only for hardware timing and state constraints |
| Code structure | Keep the existing modular structure |
| Error handling | Robust with contextual logs |
| Performance level | Medium with resource watermark tracking |

## Current Facts From The Codebase

- `xiaozhi_ws` already owns WebSocket activation, hello/server hello, session id, business-message gating, TTS state transitions, and binary downlink OPUS enqueue.
- `audio_opus_stream` already has an OPUS encoder task, decoder task, direct codec capture task, uplink enable switch, PCM ringbuffer, downlink ringbuffer, and watermark logging.
- `audio_opus_codec` keeps the uplink encoder fixed at 16 kHz, mono, 16-bit, 60 ms. That means each valid uplink PCM frame is 1920 bytes.
- `bsp_audio_open_with_sample_rate()` can switch the shared ES8311/I2S path between 16 kHz and 24 kHz.
- `handle_server_hello()` currently starts `audio_opus_stream` with `AUDIO_OPUS_PCM_SOURCE_EXTERNAL_FEED`, so direct microphone capture is not active.
- SW3 currently maps only `BUTTON_SINGLE_CLICK` to a queued text detect test. Long-press start and long-press release are registered at the BSP input layer but are not used by the app controller.
- `CONFIG_XIAOZHI_STAGE1_AUTO_SR_ENABLE` must remain disabled. P1 must not initialize AFE/VAD/SR.

## Design Choice

Recommended and approved behavior: TTS playback is protected. If SW3 is pressed while the state machine is speaking or waiting for TTS stop, P1 ignores the recording request and logs the reason. Abort and barge-in are P2 work.

Reasoning:

- P1 must prove the uplink audio path first. Mixing abort behavior into the same change makes failures ambiguous.
- The current downlink path is already known-good. P1 should not destabilize it.
- The server and local decoder already coordinate TTS stop back to READY. Waiting for READY before allowing another long press gives a clean multi-round baseline.

## Alternatives Considered

### Approach A: Direct codec capture with sample-rate switching

Use `AUDIO_OPUS_PCM_SOURCE_DIRECT_CODEC`, switch ES8311/I2S to 16 kHz while recording, and switch back to the server hello playback rate after `listen stop`.

Pros:

- Preserves the OPUS encoder contract exactly: 16 kHz, 60 ms, 1920 bytes.
- Avoids adding a resampler in P1.
- Uses existing capture and encoder tasks.

Cons:

- The shared codec/I2S rate must be changed at start/stop boundaries.
- Recording and playback cannot run concurrently in P1.

Decision: use this approach.

### Approach B: Capture at 24 kHz and resample to 16 kHz

Keep I2S at server playback rate and downsample mic PCM before encoding.

Pros:

- Avoids codec/I2S reconfiguration during a session.
- Better foundation for future full-duplex behavior.

Cons:

- Adds DSP/resampling code before the base uplink path is proven.
- More CPU, memory, and test surface.
- A poor resampler can corrupt STT and mask microphone or encoder problems.

Decision: defer until realtime/AEC/full-duplex becomes a real requirement.

### Approach C: Keep external feed and route microphone through SR/AFE

Use the existing `xiaozhi_ws_feed_processed_pcm()` path and let SR/AFE produce PCM.

Pros:

- May align with the future VAD/AEC architecture.

Cons:

- SR/AFE previously had initialization crash risk.
- Violates the explicit P1 constraint to keep SR/VAD disabled.

Decision: reject for P1.

## Architecture

### Input controller

`app_input_controller` should expose SW3 long-press start and long-press up as voice input events, not only single click. The callback should stay thin: decode button event, call the configured app callback, and return quickly.

Expected app-level events:

- `VOICE_SESSION_EVT_TEXT_TEST` for existing SW3 single click regression.
- `VOICE_SESSION_EVT_LISTEN_START` for SW3 long press start.
- `VOICE_SESSION_EVT_LISTEN_STOP` for SW3 long press release.

### App controller

`voice_session_task` remains the serialization point for voice button events. It calls `xiaozhi_ws_trigger_listen(XIAOZHI_WS_LISTEN_MODE_BUTTON)` on long press start and `xiaozhi_ws_stop_listen()` on long press release.

The task should not call WebSocket functions from the button callback itself. That keeps button callbacks non-blocking and avoids running connect or send operations in the input event context.

### WebSocket/session state machine

`xiaozhi_ws_trigger_listen(XIAOZHI_WS_LISTEN_MODE_BUTTON)` should become the manual listen start entry point:

1. If state is `SPEAKING` or waiting for TTS stop, return `ESP_ERR_INVALID_STATE` after logging that the request was ignored.
2. Ensure WebSocket is READY. If disconnected, start WebSocket, wait for hello/server hello, then continue.
3. Require a non-empty `session_id`.
4. Stop or drain downlink playback before changing the audio path.
5. Switch capture/playback hardware to 16 kHz for microphone capture.
6. Start or reuse `audio_opus_stream` with `AUDIO_OPUS_PCM_SOURCE_DIRECT_CODEC`.
7. Send `{"type":"listen","state":"start","mode":"manual","session_id":...}`.
8. Enable uplink and enter `LISTENING`.

`xiaozhi_ws_stop_listen()` should:

1. Disable uplink immediately.
2. Flush pending uplink PCM so no stale frame leaks into the next turn.
3. Send `listen stop`.
4. Switch the audio path back to the server hello downlink sample rate.
5. Enter `WAITING_RESPONSE` if stop was sent, otherwise `DISCONNECTED`.

Server TTS `start`, binary OPUS, and TTS `stop` keep their current responsibilities. TTS `stop` returns to READY.

### Audio stream

`audio_opus_stream` should stay long-lived for the WebSocket session when possible. It should not repeatedly open and close the OPUS pipeline for every button press unless a disconnect or sample-rate mismatch forces a restart.

Direct capture behavior:

- Capture task sleeps while uplink is disabled.
- When uplink is enabled, it reads exactly `s_stream.pcm_frame_bytes`.
- For 16 kHz, 60 ms, mono, 16-bit, this is 1920 bytes.
- Each captured frame is fed into the encoder ringbuffer.
- Encoder sends binary OPUS through the existing `send_cb`.

### PCM diagnostics

P1 needs lightweight runtime validation of microphone PCM. Direct capture should log a small rolling summary:

- frame count
- input byte length
- min sample
- max sample
- RMS
- zero-sample count or all-zero frame flag
- clipping/saturation count

Log the first three frames and then one window at a configurable interval, such as every 500 ms or every 8 frames. The goal is to quickly identify all-zero capture, I2S format mismatch, saturation, or meaningless noise without flooding logs.

## Sample-Rate Contract

This is the critical P1 risk.

Uplink:

- OPUS encoder remains fixed at 16 kHz.
- Frame duration remains 60 ms.
- One input frame must be 1920 bytes.
- I2S RX must therefore be 16 kHz during manual recording unless a real resampler is added.

Downlink:

- Server hello may announce 24 kHz.
- OPUS decoder output and speaker playback should follow server hello.
- The existing downlink path already supports `decoder_output_sample_rate`.

Boundary rule:

- Before `listen start`, switch codec/I2S to 16 kHz.
- After `listen stop`, switch codec/I2S back to the resolved server playback rate, normally 24 kHz.
- While `LISTENING`, downlink playback is not expected. If downlink binary arrives during listening, the state machine should disable uplink and move to speaking, as it already does for received binary OPUS.

No P1 code should silently encode 24 kHz microphone PCM as 16 kHz OPUS.

## Error Handling

- If WebSocket is disconnected on long press, reconnect, perform hello, wait for READY, then send listen start.
- If READY wait fails, do not start capture.
- If sample-rate switch to 16 kHz fails, do not send listen start.
- If `listen start` send fails, disable uplink and return to DISCONNECTED or READY depending on connection state.
- If OPUS binary send fails, stop session audio I/O and clean the WebSocket client through the existing write-failure path.
- If `listen stop` is called outside `LISTENING` or `WAKE_DETECTED`, it should be idempotent and return OK.
- If TTS is speaking, manual listen start is ignored in P1.

## Stability Observability

At minimum, logs should cover:

- `listen start manual` payload and state transition.
- audio path sample rate before and after manual recording.
- first captured PCM statistics.
- OPUS tx frame count and encoded bytes.
- `listen stop` payload and transition to `WAITING_RESPONSE`.
- STT text received.
- TTS start/stop and READY return.
- heap, PSRAM, and task stack watermark at READY, listen start, listen stop, TTS start, and TTS stop.

Existing `audio_opus_stream_log_watermarks()` should be reused rather than adding a parallel diagnostics system.

## Static And Build Verification

Add or extend static guardrails to check:

- SW3 long-press start maps to a queued listen start event.
- SW3 long-press release maps to a queued listen stop event.
- `xiaozhi_ws_trigger_listen()` sends manual listen start instead of only wake detection.
- WebSocket listen start uses direct codec capture for P1.
- Manual listen start protects the speaking state.
- Capture path has PCM statistics logging.
- Uplink encoder sample rate remains 16 kHz.
- Downlink decoder sample rate still follows server hello.
- SR auto init remains disabled.

Build verification should use the project ESP-IDF environment:

```powershell
$env:IDF_PYTHON_ENV_PATH='E:/Espressif/python_env/idf5.3_py3.11_env'
. 'E:/Espressif/frameworks/esp-idf-v5.3.1/export.ps1'
idf.py build
python tools/check_p0_voice_loop.py
```

If the P1 guardrail is implemented as a new script, run that script as well.

## Board Acceptance

P1 is accepted only when all of these are observed on hardware:

1. Boot reaches WebSocket READY with SR auto init disabled.
2. SW3 long press sends `listen start` with `mode=manual`.
3. I2S RX captures non-zero microphone PCM at 16 kHz with plausible min/max/RMS.
4. OPUS encoder sends 60 ms binary frames through WebSocket.
5. SW3 release sends `listen stop`.
6. Server returns STT text from spoken audio.
7. Server returns TTS JSON and binary OPUS.
8. Board plays TTS through ES8311/I2S/PA.
9. TTS stop returns to READY.
10. A second long press starts another manual turn without reboot.

Stability pass for P1/P2 boundary:

- 10 consecutive manual turns without heap collapse.
- No repeated OPUS encoder/decoder open/close leak pattern.
- WebSocket idle close can be recovered by the next SW3 long press.

## Non-Goals

- No SR wake word.
- No VAD auto endpointer.
- No AEC or realtime full-duplex.
- No barge-in during TTS.
- No MCP/IOT/UI/Camera/LCD expansion.
- No 24 kHz to 16 kHz resampler in P1.

