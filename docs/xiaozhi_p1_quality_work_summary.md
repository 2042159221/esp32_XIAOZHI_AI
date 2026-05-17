# Xiaozhi P1 质量修复工作总结

日期：2026-05-17

分支：`xiaozhi-p1-quality-fixes`

目标：收口 P1 手动长按上行语音链路的状态机、音频采集质量、provisioning 生命周期和 Wi-Fi 驱动 warning 排查，不扩展 SR/VAD/UI/MCP。

## 已完成工作

- 修复 manual listen start 竞态：WebSocket 进入 `LISTENING` 后才允许 Opus binary 上行，避免 READY 阶段丢首批帧。
- 增加 pending PTT/button down 状态保护：READY 前长按只记录 pending，READY 前松手会取消 pending，不再事后启动录音。
- app controller 不再阻塞等待 WebSocket connect/hello/READY，按键侧只投递请求，由 session 状态机继续处理。
- `WAITING_RESPONSE` 增加超时恢复，短录音或少帧录音不再长期卡死，P0/P1 可再次触发。
- 增加最短录音保护：少于 8 帧或低于 800 ms 的录音走短等待或直接恢复，避免无效上行拖住状态机。
- 修复 P1 播放阶段误开 uplink encoder，encoder 只在上行启用时打开。
- 关闭 `BUTTON_LONG_PRESS_HOLD` 高频日志，只保留关键按键事件。
- 收口 I2S channel disable 的 `ESP_ERR_INVALID_STATE` 噪声，避免误导链路判断。
- 调整 ES8311 mic gain 默认值并保留 PCM 诊断日志，便于继续看 RMS、peak、zero/clip。
- 增加 provisioning stop/deinit 生命周期日志和残留 scan stop 保护。
- 固定 STA Wi-Fi country 为 `CN` 并关闭 802.11d 自动覆盖，用于验证 `wifi:exceed max band, 2g, ngroup:13` 是否来自国家域/信道规则。

## 用户手动验证结果

- 点按 P0：可正常发送“介绍自己”。
- 长按 P1：可正常录音、识别并收到回复。
- TTS 播放时按按钮：不会打断播放，符合当前 P1 预期。

## 自动验证结果

- `python tools/check_stage1_lifecycle.py` 通过。
- `python tools/check_p0_voice_loop.py` 通过。
- `python tools/check_p1_manual_uplink_voice.py` 通过。
- `idf.py build` 通过。

构建环境提示：ESP-IDF 输出过 Python venv 诊断 warning，但编译和链接成功，不是工程构建失败。

## 关键提交

- `0e960ba wip(voice): add p1 quality guardrails`
- `70b8b3b fix(voice): enter listening before enabling uplink`
- `e85ba6e fix(input): stop logging long press hold events`
- `d6d0e95 fix(audio): defer opus encoder until uplink starts`
- `360a829 fix(audio): guard i2s channel disable state`
- `dd9a765 fix(audio): configure es8311 microphone gain`
- `a054ce3 chore(config): persist p1 microphone gain default`
- `0c19ac5 wip(xiaozhi): add p1 state machine guardrails`
- `77a4614 fix(xiaozhi): harden p1 manual listen state machine`
- `661a622 fix(xiaozhi): ignore manual listen while waiting response`
- `9d1c786 wip(provisioning): add lifecycle scan guardrails`
- `701ddf1 fix(provisioning): log and stop residual wifi scans before stage1`
- `5d8cc56 test(provisioning): align lifecycle guardrails with current logs`
- `7955579 fix(wifi): pin sta country domain`

## 一流问题

这里的“一流问题”按当前最高优先级理解：它们不是已阻塞 P1 基本链路的问题，但会影响后续稳定性、噪声判断或产品级验收。

1. `wifi:exceed max band, 2g, ngroup:13` 仍需刷机实测确认

   应先看启动日志是否出现：

   ```text
   wifi country configured cc=CN schan=1 nchan=13 ... policy=MANUAL ieee80211d=disabled
   ```

   如果该日志出现后 warning 消失，根因基本可确认是默认 world-safe/802.11d 自动国家域与当前 2.4G 环境不匹配。如果 warning 仍存在，应将路由器 2.4G 固定到 channel `1/6/11`、20 MHz，关闭自动信道/40 MHz/区域优化后复测。

2. 麦克风质量还需要多轮稳定性数据

   当前链路已可识别并回复，但还需要继续观察正常说话、贴近说话、静音三组 PCM RMS/peak 差异，确认没有偶发低电平、削顶或 slot/channel 异常。

3. 内部内存余量需要继续监控

   P1 可运行，但后续若打开 SR/VAD/AEC/UI 叠加功能，内部内存会更紧。继续保留 heap 日志，优先避免无用 encoder、过量日志和内部 RAM ringbuffer 扩张。

4. P1 仍不实现播放打断

   当前播放期间按钮不打断是预期行为。若后续要做 barge-in，需要单独设计全双工、播放取消、上行重入和服务端 cancel 协议，不能混入当前 P1 稳定性收口。

## 合并建议

本分支已达到“P1 基础语音链路可用、状态机不再卡死、自动检查和构建通过”的合并标准。合并到 `main` 后，下一轮重点应是硬件串口复测 Wi-Fi warning 和多轮语音质量数据，不建议在同一轮继续扩展 SR/VAD/UI/MCP。
