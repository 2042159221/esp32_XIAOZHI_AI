# 音频板级测试计划

## 1. 目标

本计划用于验证 ESP32-S3 板级音频链路是否满足最小可用性要求，且测试代码不直接写死在正式业务流程中。

覆盖项：

- 音频 I2C 总线探测
- 扬声器 1 kHz 测试音播放
- 麦克风 `min/max/rms` 采样日志
- 可选本地 `Mic -> Speaker` 回环
- WebSocket PCM echo 联调

## 2. 测试结构

- 正式固件：
  - 新增 `main/services/audio/audio_pcm_diag.c/h`
  - 默认不自动播放测试音
  - 默认不自动开启本地回环
  - 仅在 `menuconfig -> Xiaozhi Agent Stage1 Configuration -> Build audio diagnostics helpers` 打开后编译诊断函数
- 独立测试 app：
  - `test_apps/audio_board_test/`
  - 用于真机音频链路验证
  - 可通过 Kconfig 逐项启用测试步骤
- PC 端工具：
  - `tools/ws_audio_echo_server.py`
  - 用于回传设备上传的 binary PCM

## 3. 前置条件

- 硬件：
  - 目标板已连接扬声器和麦克风
  - USB 串口稳定
  - 板子与 PC 在同一局域网
- 软件：
  - ESP-IDF 5.3.1 环境已就绪
  - Python 安装 `websockets`

```powershell
python -m pip install websockets
```

## 4. 构建与烧录

### 4.1 正式固件

正式固件默认不会执行任何音频测试。若只想保留诊断函数供后续人工调用，可执行：

```powershell
idf.py menuconfig
```

进入：

- `Xiaozhi Agent Stage1 Configuration`
- 打开 `Build audio diagnostics helpers`

注意：即使打开该选项，正式固件仍不会自动播放测试音，也不会自动开启本地回环。

### 4.2 独立测试 app

先配置测试参数：

```powershell
idf.py -C test_apps/audio_board_test menuconfig
```

重点配置项：

- `Audio Board Test -> Run WebSocket PCM echo test`
- `Audio Board Test -> WebSocket echo server URI`
- `Audio Board Test -> Audio diagnostics local loopback duration (ms)`
- `Audio Board Test -> Audio diagnostics local loopback digital gain`
- `Audio Board Test -> Audio diagnostics output volume`
- `Xiaozhi WiFi STA Configuration -> WiFi STA SSID`
- `Xiaozhi WiFi STA Configuration -> WiFi STA Password`

然后构建、烧录并抓完整串口日志：

```powershell
New-Item -ItemType Directory -Force logs | Out-Null
idf.py -C test_apps/audio_board_test build flash monitor 2>&1 | Tee-Object -FilePath logs\audio_board_test_serial.log
```

## 5. PC 端 echo server

启动并抓完整 PC server 日志：

```powershell
python tools/ws_audio_echo_server.py --host 0.0.0.0 --port 8765 2>&1 | Tee-Object -FilePath logs\ws_audio_echo_server.log
```

若设备测试 app 中的 URI 为默认值，则需在 `menuconfig` 中改成你的 PC 局域网地址，例如：

```text
ws://192.168.1.23:8765
```

## 6. 执行步骤

### 步骤 A：I2C 扫描

预期串口日志关键字：

- `start audio I2C scan`
- `I2C device detected at 0x18` 或 `0x19`
- `audio I2C scan completed`

通过标准：

- 至少发现 1 个 I2C 设备
- ES8311 地址命中 `0x18` 或 `0x19`

### 步骤 B：1 kHz 测试音

预期串口日志关键字：

- `play 1 kHz tone for 500 ms`
- `1 kHz tone playback finished`

通过标准：

- 扬声器可清晰听到约 0.5 秒稳定纯音
- 无持续爆音、无长时间卡住

### 步骤 C：Mic min/max/rms

预期串口日志关键字：

- `print mic RMS`
- `mic window=... min=... max=... rms=...`

执行方法：

- 先保持环境安静 1 到 2 秒
- 再靠近麦克风持续说话 2 到 3 秒

通过标准：

- 安静时 `rms` 维持在较低水平
- 说话时 `rms` 明显上升，建议至少达到静音基线的 3 倍
- `min/max` 摆幅在说话时明显变大

### 步骤 D：本地 Mic -> Speaker 回环

默认关闭。如需执行，在 test app `menuconfig` 中打开：

- `Audio Board Test -> Run local mic-to-speaker loopback`

预期串口日志关键字：

- `start local mic-to-speaker loopback`
- `loopback window=... in_min=... in_max=... in_rms=... out_min=... out_max=... out_rms=... clipped=...`
- `local loopback finished`

通过标准：

- 能听到自身说话被设备实时放出
- 说话时 `in_rms` 明显高于安静时，且 `out_rms` 随 `digital_gain` 放大
- `clipped` 偶发少量可接受；若持续大量增长，说明增益过大，应降低 `local loopback digital gain`
- 无明显持续阻塞
- 若出现啸叫，属于声学环境问题，不直接判定软件失败，但应缩短距离或降低音量复测

### 步骤 E：WebSocket PCM echo

前提：

- PC 端 `tools/ws_audio_echo_server.py` 已启动
- test app 已配置正确的 `ws://<PC-IP>:8765`

预期设备串口日志关键字：

- `========== WIFI STA CONNECTED ==========`
- `WebSocket connected`
- `WS echo stats elapsed_ms=... tx_frames=... rx_frames=...`
- `WS echo test finished`

预期 PC server 日志关键字：

- `WebSocket PCM echo server listening`
- `client connected`
- `binary frame client=... count=... len=...`
- `client summary`

通过标准：

- 设备端 `tx_frames > 0`
- 设备端 `rx_frames > 0`
- `dropped_rx=0` 为优先目标，非零时需复查网络或播放侧拥塞
- 扬声器可听到服务器回传后的音频
- PC 端日志持续收到 binary frame，且累计 `bytes_in/bytes_out` 持续增长

## 7. 失败判定

- I2C 扫描无设备：优先排查电源、I2C 引脚、ES8311 地址绑位
- 1 kHz 音播放失败：优先排查 PA、喇叭接线、I2S 引脚
- Mic RMS 不变化：优先排查 MIC 供电、I2S DI、codec ADC 配置
- WS echo 无回包：优先排查 PC IP、路由、防火墙、URI 配置、局域网隔离

## 8. 交付日志

提交测试结果时至少提供：

- `logs/audio_board_test_serial.log`
- `logs/ws_audio_echo_server.log`

建议补充：

- 测试时使用的 `sdkconfig`
- 实际板型、供电方式、扬声器/麦克风接线说明
