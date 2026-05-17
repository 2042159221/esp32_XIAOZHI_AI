# XIAOZHI AI Voice Loop 阶段进度报告

## 1. 当前总体结论

- 目标是实现完整 XIAOZHI AI 对话交流。
- 当前尚未完成完整语音对话闭环。
- Wi-Fi、OTA、WebSocket 握手已经基本打通。
- ES8311 Codec / I2S 初始化已经通过。
- 当前核心阻塞仍是 Opus encoder 初始化失败，日志表现为 ret:-7。最近一轮真机日志里也观察到 Opus encoder 曾经 init OK 后 decoder ret:-7，因此下一阶段需要复核 ret:-7 的准确发生点，不能把 Opus 编解码链路视为完成。
- ret:-7 高概率仍与内部 SRAM/Opus 内部分配/PSRAM malloc 策略/任务栈与音频缓冲资源竞争有关。

## 2. 已完成进度

### Wi-Fi 配网与 STA 连接

- 当前状态：部分完成。
- 关键日志证据：
  - `wifi_sta_service: ========== WIFI STA CONNECTED ==========`
  - `wifi_sta_service: got ip: 192.168.39.76, netmask: 255.255.255.0, gw: 192.168.39.223`
  - `prov_service: WIFI_GOT_IP_BIT set`
- 修改过的文件：
  - `main/services/provisioning/provisioning_service.c`
  - `main/app/app_controller.c`
  - `main/app/app_input_controller.c`
  - `main/app/app_input_controller.h`
  - `sdkconfig`
  - `sdkconfig.defaults`
- 剩余风险：
  - BLE/SoftAP 策略仍需收敛。
  - 重新配网路径和已配网路径仍需要更多轮真机回归，确认行为长期一致。

### provisioning 与 stage1 生命周期隔离

- 当前状态：已完成阶段性修复。
- 关键日志证据：
  - `wifi_prov_mgr: Provisioning stopped`
  - `wifi_prov_scheme_ble: BTDM memory released`
  - `prov_service: PROV_STOPPED_BIT set, PROV_DEINITED=1`
  - `prov_service: after provisioning stopped/deinit ...`
  - `prov_service: before stage1 start ...`
  - `xiaozhi_stage1: xiaozhi ota task started`
- 修改过的文件：
  - `main/services/provisioning/provisioning_service.c`
  - `main/Kconfig.projbuild`
  - `sdkconfig`
  - `sdkconfig.defaults`
  - `tools/check_stage1_lifecycle.py`
- 剩余风险：
  - 当前通过异步 finalize task 避免在 `WIFI_PROV_END` 回调里直接 deinit；还需要长时间验证异常断开、凭据失败、重复配网等路径。

### OTA HTTP 请求

- 当前状态：已完成。
- 关键日志证据：
  - `xiaozhi_ota: HTTP status=200, response_len=472`
  - `xiaozhi_ota: device is activated`
  - `xiaozhi_ota: websocket url present: yes`
  - `xiaozhi_ota: websocket token: present, len=10`
- 修改过的文件：
  - `main/app/xiaozhi_stage1.c`
  - `main/xiaozhi/xiaozhi_device.c`
  - `main/xiaozhi/xiaozhi_device.h`
- 剩余风险：
  - 网络偶发 TLS timeout 仍可能导致首次 WebSocket 连接失败，需要后续重试策略和状态恢复回归。

### WebSocket 连接与 hello 握手

- 当前状态：已完成阶段性打通。
- 关键日志证据：
  - `xiaozhi_ws: websocket connected`
  - `xiaozhi_ws: hello sent`
  - `xiaozhi_ws: state transition CONNECTED -> HELLO_SENT`
  - `xiaozhi_ws: state transition HELLO_SENT -> READY`
- 修改过的文件：
  - `main/xiaozhi/xiaozhi_ws.c`
  - `main/xiaozhi/xiaozhi_ws.h`
  - `main/xiaozhi/xiaozhi_protocol.c`
  - `main/xiaozhi/xiaozhi_protocol.h`
  - `main/CMakeLists.txt`
  - `test_apps/xiaozhi_protocol_test/`
- 剩余风险：
  - WebSocket clean close 和重连策略仍需完善。
  - 完整音频链路未通过前，WebSocket 只能算协议握手阶段完成。

### 服务器 hello 参数解析

- 当前状态：已完成阶段性实现。
- 关键日志证据：
  - `xiaozhi_ws: server hello received`
  - `xiaozhi_ws: session_id=397de211`
  - `xiaozhi_ws: server audio params format=opus sample_rate=24000 channels=1 frame_duration=60`
- 修改过的文件：
  - `main/xiaozhi/xiaozhi_protocol.c`
  - `main/xiaozhi/xiaozhi_protocol.h`
  - `main/xiaozhi/xiaozhi_ws.c`
  - `test_apps/xiaozhi_protocol_test/main/test_xiaozhi_protocol.c`
- 剩余风险：
  - 下行 24 kHz Opus 参数与本地 16 kHz Codec/I2S 播放路径仍需统一或重采样策略。

### ES8311 I2C probe

- 当前状态：已完成。
- 关键日志证据：
  - `bsp_audio: Start ES8311 I2C probe on SDA=0 SCL=1`
  - `bsp_audio: ES8311 probe result: case A, 7-bit address 0x18, codec address 0x30`
  - `Adev_Codec: Open codec device OK`
- 修改过的文件：
  - 本轮没有主要修改 BSP Codec probe 逻辑。
  - `main/services/audio/audio_opus_stream.c` 调用 `bsp_audio_open()` 进入正式音频流路径。
- 剩余风险：
  - Opus 下行播放未通过前，speaker write 的完整链路仍未验收。

### I2S TX/RX 初始化

- 当前状态：已完成。
- 关键日志证据：
  - `I2S_IF: STD: TX ... sample_rate_hz: 16000`
  - `I2S_IF: STD: RX ... sample_rate_hz: 16000`
- 修改过的文件：
  - `main/services/audio/audio_opus_stream.c`
  - `main/services/sr/xiaozhi_sr.c`
- 剩余风险：
  - 需要继续保证不会多个任务同时调用 `esp_codec_dev_read()` 读取同一个 ES8311/I2S RX。

### SR/AFE 初始化尝试

- 当前状态：有风险，当前默认已临时禁用自动初始化。
- 关键日志证据：
  - 曾出现 `Guru Meditation Error: Core 1 panic'ed (StoreProhibited)`
  - panic 点：`esp_mel_filter_init -> fbank_create -> model_create -> afe_init_vad -> afe_create_from_config -> init_afe`
  - 当前验证日志：`xiaozhi_stage1: websocket READY; SR auto init disabled`
- 修改过的文件：
  - `main/app/xiaozhi_stage1.c`
  - `main/Kconfig.projbuild`
  - `sdkconfig`
  - `sdkconfig.defaults`
  - `main/services/sr/xiaozhi_sr.c`
  - `main/services/sr/xiaozhi_sr.h`
- 剩余风险：
  - AFE/VAD 模型初始化崩溃尚未修复。
  - 后续启用 SR 前必须单独复现并定位 `esp_mel_filter_init` 空指针写来源。

### PSRAM 启用与 heap 日志增强

- 当前状态：部分完成。
- 关键日志证据：
  - `esp_psram: Found 8MB PSRAM device`
  - `esp_psram: Speed: 80MHz`
  - `esp_psram: SPI SRAM memory test OK`
  - `prov_service: ... internal free=... internal largest free block=... spiram free=... spiram largest free block=...`
  - `audio_opus_codec: before opus encoder open heap free=... internal_free=... internal_largest=... spiram_free=... spiram_largest=... task_stack_watermark=...`
- 修改过的文件：
  - `main/services/provisioning/provisioning_service.c`
  - `main/services/audio/audio_opus_codec.c`
  - `main/services/sr/xiaozhi_sr.c`
  - `sdkconfig`
  - `sdkconfig.defaults`
- 剩余风险：
  - 当前配置为 `CONFIG_SPIRAM_USE_CAPS_ALLOC=y`，未启用 `CONFIG_SPIRAM_USE_MALLOC`。
  - 需要确认第三方 Opus 内部 malloc 是否能进入 PSRAM。

### 任务栈迁移或内存优化尝试

- 当前状态：部分完成。
- 关键日志证据：
  - `xiaozhi_sr.c` 增加 `xTaskCreatePinnedToCoreWithCaps(... MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)` 路径。
  - `audio_opus_stream.c` 对音频 ringbuffer 和 stream task 增加 PSRAM caps 分配路径。
  - `audio_opus_codec` 增加 Opus 初始化前后的 heap/stack watermark 日志。
- 修改过的文件：
  - `main/services/sr/xiaozhi_sr.c`
  - `main/services/audio/audio_opus_stream.c`
  - `main/services/audio/audio_opus_stream.h`
  - `main/services/audio/audio_opus_codec.c`
  - `main/services/audio/audio_opus_codec.h`
- 剩余风险：
  - Opus 编解码器内部状态仍可能从内部 SRAM 分配。
  - Wi-Fi/TLS/WebSocket/LVGL/SR/Opus 并存时，内部 largest free block 仍可能不足。

## 3. 当前阻塞问题

### 3.1 Opus encoder init failed ret:-7

- 这是当前阻塞完整 AI 语音对话的主问题。
- WebSocket 已经 READY，但进入音频编码链路时失败。
- ret:-7 需要继续确认是否为 OPUS_ALLOC_FAIL 或等价内存申请失败。
- 需要继续检查 `esp_opus_enc_open` 内部 malloc 是否进入 PSRAM。
- 需要确认 `CONFIG_SPIRAM_USE_MALLOC` / `CONFIG_SPIRAM_USE_CAPS_ALLOC` 配置影响。
- 需要在 Opus 初始化前打印：
  - internal free
  - internal largest free block
  - psram free
  - psram largest free block
  - task stack watermark
- 当前代码已经加入上述 heap/stack 日志，但还没有完成 ret:-7 根因修复。
- 注意：最近一轮真机日志中出现过 `opus encoder init OK` 后 `ESP_OPUS_DEC: Fail to create decoder ret -7`，因此下一阶段需要同时复核 encoder 与 decoder 初始化失败路径，不能只看单一日志片段。

### 3.2 SR/AFE 风险

- SR/AFE 曾出现 StoreProhibited panic。
- panic 点在 `esp_mel_filter_init / fbank_create / vadnet model_create`。
- 当前代码已加入 `CONFIG_XIAOZHI_STAGE1_AUTO_SR_ENABLE` 开关，默认关闭 SR 自动初始化，避免影响 WebSocket 主链路验收。
- 后续建议继续保持默认关闭，先让系统稳定停在 WebSocket READY，再单独修 AFE/VAD 崩溃。

### 3.3 provisioning 生命周期风险

- 之前出现过 provisioning/NimBLE 未释放时启动 stage1，导致 OTA task 创建 `ESP_ERR_NO_MEM`。
- 当前已尝试改为 Wi-Fi got IP 且 provisioning stop/deinit 后再启动 stage1。
- 当前重新配网路径和已配网路径都已经看到 `PROV_STOPPED_BIT set, PROV_DEINITED=1` 与 `before stage1 start` 日志。
- 仍需要继续确保重新配网路径和已配网路径行为一致，尤其是凭据失败、重复配网、BLE 连接异常断开等边界条件。

## 4. 当前系统链路状态

| 阶段 | 状态 | 说明 |
|---|---|---|
| 配网 | 部分完成 | BLE/SoftAP 策略仍需收敛 |
| Wi-Fi STA | 已完成 | 可拿到 IP |
| OTA | 已完成 | HTTP 200，能拿 websocket url/token |
| WebSocket | 已完成 | hello sent/server hello/READY |
| Codec | 已完成 | ES8311 probe OK |
| I2S | 已完成 | TX/RX 初始化 OK |
| SR/AFE | 有风险 | 曾 panic |
| Opus Encoder | 未完成 | ret:-7 |
| 完整 AI 对话 | 未完成 | 卡在音频编码链路 |

## 5. 距离完整 XIAOZHI AI 对话还差什么

1. 修复 Opus encoder ret:-7。
2. 确认 Opus 编码器初始化成功。
3. 验证本地 PCM -> Opus encode。
4. 验证 Opus frame 通过 WebSocket 发送。
5. 验证服务器返回 Opus 音频。
6. 验证 Opus decode。
7. 验证 I2S 播放。
8. 验证按键或唤醒触发完整对话流程。
9. 稳定性测试：连续多轮对话、断网重连、内存水位。

## 6. 建议下一步排查计划

P0:

- 定位 `esp_opus_enc_open` ret:-7。
- 确认第三方 Opus 内部 malloc 是否能进入 PSRAM。
- 如果不能，优先启用 `CONFIG_SPIRAM_USE_MALLOC` 验证。
- 或在 Opus wrapper/组件源码中替换为 `heap_caps_malloc(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)`。
- 同时复核 decoder ret:-7，避免只修 encoder 后仍卡在 downlink decode 初始化。

P1:

- 给 SR 自动初始化加 Kconfig 开关。
- 先让系统稳定停在 WebSocket READY。
- 再逐步启用 SR/Opus。

P2:

- 优化 Wi-Fi/TLS/WebSocket/LVGL/SR/Opus 的任务栈和 buffer 预算。

## 7. 本次提交内容

根据当前 `git diff --stat` 和未跟踪文件列表，本次提交包含：

- 新增 XIAOZHI WebSocket JSON 协议模块：
  - `main/xiaozhi/xiaozhi_protocol.c`
  - `main/xiaozhi/xiaozhi_protocol.h`
  - `test_apps/xiaozhi_protocol_test/`
- 重构 WebSocket 协议流程：
  - 连接后发送 hello JSON。
  - 等待 server hello。
  - 解析 `session_id` 和 server audio params。
  - 增加 READY/LISTENING/SPEAKING/ERROR 等状态处理。
  - 增加 Authorization、Protocol-Version、Device-Id、Client-Id header 配置。
- 新增正式 Opus stream 雏形：
  - `main/services/audio/audio_opus_stream.c`
  - `main/services/audio/audio_opus_stream.h`
  - 上行 PCM 聚合、Opus encode、WebSocket binary 发送路径。
  - 下行 binary Opus 入队、异步 decode、speaker write 路径。
  - 增加 tx/rx/drop/free heap 日志。
- 增强 Opus codec 诊断：
  - 初始化前后打印 internal/PSRAM heap、largest block、task stack watermark。
  - 记录 encoder/decoder open 失败点。
- 调整 provisioning 生命周期：
  - 增加 `WIFI_GOT_IP_BIT`、`PROV_STOPPED_BIT`、`s_prov_deinited` 门控。
  - 避免 stage1 与 BLE provisioning 并发。
  - `WIFI_PROV_END` 后通过 finalize task 统一 deinit/start gate，避免在 manager 回调里直接 deinit。
  - 将 `CONFIG_APP_PROV_STOP_DELAY_MS` 默认值从 10000 调整为 500。
- 调整 stage1 启动：
  - OTA 激活后启动 WebSocket。
  - 等待 WebSocket READY。
  - 新增 `CONFIG_XIAOZHI_STAGE1_AUTO_SR_ENABLE`，默认关闭 SR 自动初始化。
- 增加按键触发：
  - SW3 单击调用 `xiaozhi_ws_trigger_listen(XIAOZHI_WS_LISTEN_MODE_BUTTON)`。
- 增强 SR 路径：
  - 增加回调结构。
  - 增加 SR feed/detect task 的 PSRAM task stack 创建尝试。
  - 增加 heap/stack 日志。
- 增强设备标识：
  - 增加 UUID/MAC/IP/Wi-Fi 信息相关辅助能力，用于 OTA/WebSocket header 和日志。
- 配置变更：
  - `sdkconfig` 和 `sdkconfig.defaults` 明确保持 Wi-Fi SSID/PASSWORD 为空。
  - 启用 PSRAM Quad/80 MHz/caps alloc 相关配置。
  - 保留 `audio_board_test`，未删除板级测试工程。

以下为当时未完成且不得误判为完成的事项；2026-05-17 P0 文本闭环验收更新见下一节：

- 完整 XIAOZHI AI 对话交流尚未完成。
- Opus ret:-7 尚未修复。
- SR/AFE panic 尚未根治。
- 服务器返回 Opus 音频后的 decode/playback 闭环尚未验收通过。

## P0 Text Voice Loop Verification - 2026-05-17

- Build command: `idf.py fullclean`; `idf.py build`。
- Flash/monitor command: `idf.py -p COM16 flash monitor`。
- Server hello sample_rate: `format=opus sample_rate=24000 channels=1 frame_duration=60`，已触发 `codec opened sample_rate=24000` 和 `opus decoder ready output_sample_rate=24000 decoded_frame_bytes=2880`。
- SW3 detect text sent: 已发送，日志为 `listen detect payload={"session_id":"0e1e0f59","type":"listen","state":"detect","text":"你好，请介绍你自己"}`，随后 `state transition READY -> WAITING_RESPONSE`。
- TTS JSON observed: 已观察到 `tts state=start`、多条 `tts state=sentence_start`、`stt text=你好，请介绍你自己`、`tts state=stop`，并确认 `state transition SPEAKING -> READY`。
- Binary OPUS frames observed: 已观察到大量 `binary opus received len=...`，片段中长度约 `77` 到 `274` 字节；重连后第二轮 TTS 的 `rx_frames=481 decoded_frames=481`，未见 downlink drop。
- Speaker playback result: 用户确认板子已经可以播放声音；日志多次出现 `speaker playback OK`，`playback_failures=0`。
- READY idle disconnect recovery: 已通过。日志先出现 `state transition READY -> DISCONNECTED`，随后 SW3 单击触发 `cleanup stale websocket client before reconnect`、`DISCONNECTED -> CONNECTING -> WS_CONNECTED -> HELLO_SENT -> READY`，再发送 `listen detect payload={"session_id":"81a3c6b9","type":"listen","state":"detect","text":"你好，请介绍你自己"}` 并完成 TTS 播放。
- Internal heap low watermark: 本次片段中 `minimum_free_heap` 低点约 `8142016` bytes，`internal_free` 低点约 `27435` bytes，`internal_largest` 低点约 `7680` bytes。
- PSRAM free after TTS: `TTS stop heap` 显示 `spiram_free=8153712` bytes，`spiram_largest=7995392` bytes。
- Audio task stack watermark: 下行播放期间 `decoder_stack_free_bytes` 低点约 `15892`，`encoder_stack_free_bytes` 约 `48208`；`capture_stack_free_bytes=0`，因为 P0 文本闭环未启用上行采集。

最新结论：P0 的 `SW3 单击 -> 文本 detect -> 服务端 TTS -> 24 kHz OPUS 解码 -> ES8311/I2S/PA 播放` 已经通过实机验证；`tts stop -> READY` 和 READY 空闲断开后的再次 SW3 自动重连 detect 也已通过。
