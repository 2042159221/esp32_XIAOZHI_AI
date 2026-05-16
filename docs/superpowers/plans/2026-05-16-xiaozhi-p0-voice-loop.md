# XiaoZhi P0 Voice Loop Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the P0 acceptance path: SW3 single click connects WebSocket if needed, waits for hello READY, sends a real `listen/detect/text` request, receives TTS JSON plus binary OPUS, decodes at the server sample rate, and plays through ES8311/NS4150B.

**Architecture:** Keep SR/VAD disabled for P0. Add the missing protocol builder, make `xiaozhi_ws` own WebSocket/session sequencing, make playback sample rate follow server hello, and add fixed diagnostics at lifecycle checkpoints. Do not add Camera, LCD framebuffer, MCP/IOT, background SR, or continuous recording in this plan.

**Tech Stack:** ESP-IDF v5.3.1, ESP32-S3, FreeRTOS, `esp_websocket_client`, cJSON, Espressif OPUS encoder/decoder, `esp_codec_dev`, ES8311, NS4150B, Unity test app.

---

## Configuration

| 配置项 | 选择 |
| --- | --- |
| 使用子 | 禁用；执行阶段可按用户选择启用 subagent-driven development |
| 编程范式 | 事件驱动 + 过程式状态机 |
| 语言 | C for firmware, Python for static guardrail script |
| 项目类型 | ESP-IDF embedded voice application |
| 评论风格 | 极简，只解释硬件时序和状态机约束 |
| 代码结构 | 保持现有模块化结构，不在 P0 大拆分目录 |
| 错误处理策略 | 稳健 + 上下文日志 |
| 性能优化级别 | 中 + 可验收资源水位 |

## Ground Rules

- Current worktree already has `M sdkconfig`. Treat it as user state. Do not stage or commit `sdkconfig` unless the active task explicitly changes it.
- After every task that changes files, run `git status`, `git diff`, `git add <task files>`, and `git commit`.
- Use `wip(<scope>): ...` only if a task cannot build or its verification cannot run after code changes.
- Keep `CONFIG_XIAOZHI_STAGE1_AUTO_SR_ENABLE` disabled through this plan.
- Use the ESP-IDF environment from the project instructions when `idf.py` is unavailable:

```powershell
$env:IDF_PYTHON_ENV_PATH='E:/Espressif/python_env/idf5.3_py3.11_env'
. 'E:/Espressif/frameworks/esp-idf-v5.3.1/export.ps1'
```

## Files

- Modify: `main/xiaozhi/xiaozhi_protocol.h`
  - Add a first-class builder for `listen` `detect` text JSON.
- Modify: `main/xiaozhi/xiaozhi_protocol.c`
  - Build `{"session_id":"","type":"listen","state":"detect","text":"..."}` and allow empty `session_id` only for detect text.
- Modify: `test_apps/xiaozhi_protocol_test/main/test_xiaozhi_protocol.c`
  - Add Unity coverage for detect text JSON and empty-text rejection.
- Modify: `main/xiaozhi/xiaozhi_ws.h`
  - Expand connection/session states so callers can distinguish connecting, hello-sent, ready, speaking, and closing.
- Modify: `main/xiaozhi/xiaozhi_ws.c`
  - Send real detect text, enforce READY-before-business-message, transition state predictably, consume server hello sample rate, and log lifecycle watermarks.
- Modify: `main/bsp/audio/bsp_audio.h`
  - Expose sample-rate-aware codec open/reopen helpers.
- Modify: `main/bsp/audio/bsp_audio.c`
  - Reconfigure ES8311/I2S to 16 kHz or 24 kHz before playback.
- Modify: `main/services/audio/audio_opus_stream.h`
  - Expose stream runtime diagnostics.
- Modify: `main/services/audio/audio_opus_stream.c`
  - Open playback at `decoder_output_sample_rate`, remove the 16 kHz playback assertion, and standardize task/heap logs.
- Modify: `main/app/app_controller.c`
  - Make SW3 single click the explicit P0 text test event and keep it queued through the controller task.
- Modify: `tools/check_p0_voice_loop.py`
  - Add static checks for real detect text, server sample-rate playback, session state progression, and diagnostics hooks.

## Task 1: Protocol Detect Text JSON

**Files:**
- Modify: `main/xiaozhi/xiaozhi_protocol.h`
- Modify: `main/xiaozhi/xiaozhi_protocol.c`
- Test: `test_apps/xiaozhi_protocol_test/main/test_xiaozhi_protocol.c`

- [ ] **Step 1: Write failing Unity tests**

Add these test cases after `listen start and stop json include session`:

```c
TEST_CASE("listen detect json carries text and permits empty session", "[xiaozhi_protocol]")
{
    char *json = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, xiaozhi_protocol_build_listen_detect_json("", "你好，请介绍你自己", &json));
    TEST_ASSERT_NOT_NULL(json);

    cJSON *root = cJSON_Parse(json);
    TEST_ASSERT_NOT_NULL(root);
    assert_json_string(root, "session_id", "");
    assert_json_string(root, "type", "listen");
    assert_json_string(root, "state", "detect");
    assert_json_string(root, "text", "你好，请介绍你自己");

    cJSON_Delete(root);
    cJSON_free(json);
}

TEST_CASE("listen detect json rejects empty text", "[xiaozhi_protocol]")
{
    char *json = NULL;

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, xiaozhi_protocol_build_listen_detect_json("sid-1", NULL, &json));
    TEST_ASSERT_NULL(json);

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, xiaozhi_protocol_build_listen_detect_json("sid-1", "", &json));
    TEST_ASSERT_NULL(json);
}
```

- [ ] **Step 2: Run test build and verify failure**

Run:

```powershell
idf.py -C test_apps/xiaozhi_protocol_test build
```

Expected: FAIL at compile time with an undeclared `xiaozhi_protocol_build_listen_detect_json`.

- [ ] **Step 3: Add the public prototype**

In `main/xiaozhi/xiaozhi_protocol.h`, add this declaration after `xiaozhi_protocol_build_listen_stop_json`:

```c
esp_err_t xiaozhi_protocol_build_listen_detect_json(const char *session_id, const char *text, char **out_json);
```

Add this compatibility macro near the existing `build_listen_*` macros:

```c
#define build_listen_detect_json xiaozhi_protocol_build_listen_detect_json
```

- [ ] **Step 4: Add an optional-session helper**

In `main/xiaozhi/xiaozhi_protocol.c`, add this helper after `add_session`:

```c
static esp_err_t add_session_optional(cJSON *root, const char *session_id)
{
    ESP_RETURN_ON_FALSE(root != NULL, ESP_ERR_INVALID_ARG, TAG, "root is required");
    const char *value = session_id != NULL ? session_id : "";
    ESP_RETURN_ON_FALSE(add_string(root, "session_id", value), ESP_ERR_NO_MEM, TAG, "add optional session_id failed");
    return ESP_OK;
}
```

- [ ] **Step 5: Implement detect JSON builder**

In `main/xiaozhi/xiaozhi_protocol.c`, add this function after `xiaozhi_protocol_build_listen_stop_json`:

```c
esp_err_t xiaozhi_protocol_build_listen_detect_json(const char *session_id, const char *text, char **out_json)
{
    ESP_RETURN_ON_FALSE(out_json != NULL, ESP_ERR_INVALID_ARG, TAG, "out_json is NULL");
    *out_json = NULL;
    ESP_RETURN_ON_FALSE(text != NULL && text[0] != '\0', ESP_ERR_INVALID_ARG, TAG, "detect text is required");

    cJSON *root = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(root != NULL, ESP_ERR_NO_MEM, TAG, "create listen detect root failed");

    esp_err_t err = add_session_optional(root, session_id);
    if (err == ESP_OK &&
        (!add_string(root, "type", "listen") ||
         !add_string(root, "state", "detect") ||
         !add_string(root, "text", text))) {
        err = ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) {
        err = print_json(root, out_json);
    }

    cJSON_Delete(root);
    return err;
}
```

- [ ] **Step 6: Run test build and verify pass**

Run:

```powershell
idf.py -C test_apps/xiaozhi_protocol_test build
```

Expected: PASS build with no undeclared symbol.

- [ ] **Step 7: Commit**

Run:

```powershell
git status
git diff -- main/xiaozhi/xiaozhi_protocol.h main/xiaozhi/xiaozhi_protocol.c test_apps/xiaozhi_protocol_test/main/test_xiaozhi_protocol.c
git add main/xiaozhi/xiaozhi_protocol.h main/xiaozhi/xiaozhi_protocol.c test_apps/xiaozhi_protocol_test/main/test_xiaozhi_protocol.c
git commit -m "feat(protocol): add listen detect text json builder" -m "修改了什么: 新增 listen detect text JSON 构造接口和 Unity 覆盖。" -m "为什么修改: SW3 P0 文本闭环必须把实际文本发送给小智服务端，不能只进入唤醒状态。" -m "影响范围: main/xiaozhi 协议层、xiaozhi_protocol_test；目标板 ESP32-S3，SDK ESP-IDF v5.3.1，RTOS FreeRTOS。" -m "编译或测试结果: idf.py -C test_apps/xiaozhi_protocol_test build 通过。"
```

## Task 2: Real SW3 Detect Text Send

**Files:**
- Modify: `main/xiaozhi/xiaozhi_ws.c`
- Modify: `main/app/app_controller.c`
- Test: `tools/check_p0_voice_loop.py`

- [ ] **Step 1: Add static check failure first**

In `tools/check_p0_voice_loop.py`, extend `check_voice_session_task` with these requirements:

```python
    require("VOICE_SESSION_EVT_TEXT_TEST" in controller,
            "app_controller.c must name SW3 single click as VOICE_SESSION_EVT_TEXT_TEST", failures)
    require("你好，请介绍你自己" in controller,
            "app_controller.c must keep the P0 detect text literal visible", failures)
```

Add this new checker:

```python
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
```

Call it from `main()` before `check_voice_session_task(failures)`:

```python
    check_detect_text_request(failures)
```

- [ ] **Step 2: Run guardrail and verify failure**

Run:

```powershell
python tools/check_p0_voice_loop.py
```

Expected: FAIL with messages that `xiaozhi_ws_trigger_detect_text` ignores text and does not build detect JSON.

- [ ] **Step 3: Make app controller event explicit**

In `main/app/app_controller.c`, replace the voice event enum with:

```c
typedef enum {
    VOICE_SESSION_EVT_TEXT_TEST = 0,
} voice_session_evt_t;
```

Add this near the queue constants:

```c
#define XIAOZHI_P0_TEXT_TEST "你好，请介绍你自己"
```

In `voice_trigger_cb`, replace the queued value:

```c
const voice_session_evt_t evt = VOICE_SESSION_EVT_TEXT_TEST;
```

In `voice_session_task`, replace the switch case with:

```c
case VOICE_SESSION_EVT_TEXT_TEST: {
    ESP_LOGI(TAG, "SW3 P0 text test trigger detected");
    esp_err_t err = xiaozhi_ws_trigger_detect_text(XIAOZHI_P0_TEXT_TEST);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "button detect ignored: %s", esp_err_to_name(err));
    }
    break;
}
```

- [ ] **Step 4: Replace `xiaozhi_ws_trigger_detect_text`**

In `main/xiaozhi/xiaozhi_ws.c`, replace the current implementation:

```c
esp_err_t xiaozhi_ws_trigger_detect_text(const char *text)
{
    (void)text;
    return xiaozhi_ws_on_wake_detected();
}
```

with:

```c
esp_err_t xiaozhi_ws_trigger_detect_text(const char *text)
{
    ESP_RETURN_ON_FALSE(text != NULL && text[0] != '\0', ESP_ERR_INVALID_ARG, TAG, "detect text is empty");

    if (s_ws_state == XIAOZHI_WS_STATE_SPEAKING || s_waiting_tts_stop) {
        ESP_LOGW(TAG, "detect text ignored while speaking state=%s", state_name(s_ws_state));
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ensure_websocket_ready();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "detect text websocket not ready: %s", esp_err_to_name(err));
        return err;
    }

    if (s_ws_state != XIAOZHI_WS_STATE_READY &&
        s_ws_state != XIAOZHI_WS_STATE_WAKE_DETECTED &&
        s_ws_state != XIAOZHI_WS_STATE_WAITING_RESPONSE) {
        ESP_LOGW(TAG, "detect text invalid state=%s", state_name(s_ws_state));
        return ESP_ERR_INVALID_STATE;
    }

    (void)audio_opus_stream_set_uplink_enabled(false);
    audio_opus_stream_flush();

    char *json = NULL;
    ESP_RETURN_ON_ERROR(xiaozhi_protocol_build_listen_detect_json(s_session_id, text, &json), TAG, "build listen detect failed");

    err = send_text_json(json, "listen detect");
    if (err == ESP_OK) {
        s_waiting_tts_stop = true;
        set_state(XIAOZHI_WS_STATE_WAITING_RESPONSE);
        log_heap_stats("listen detect sent");
    } else {
        set_state(XIAOZHI_WS_STATE_DISCONNECTED);
    }
    return err;
}
```

- [ ] **Step 5: Log outgoing listen payloads**

In `send_text_json`, add this before `esp_websocket_client_send_text`:

```c
    if (label != NULL && strncmp(label, "listen", 6) == 0) {
        ESP_LOGI(TAG, "%s payload=%s", label, json);
    }
```

Expected serial evidence after SW3:

```text
listen detect payload={"session_id":"...","type":"listen","state":"detect","text":"你好，请介绍你自己"}
listen detect sent
state transition READY -> WAITING_RESPONSE
```

- [ ] **Step 6: Verify**

Run:

```powershell
python tools/check_p0_voice_loop.py
idf.py build
```

Expected: guardrail PASS and firmware build PASS.

- [ ] **Step 7: Commit**

Run:

```powershell
git status
git diff -- main/xiaozhi/xiaozhi_ws.c main/app/app_controller.c tools/check_p0_voice_loop.py
git add main/xiaozhi/xiaozhi_ws.c main/app/app_controller.c tools/check_p0_voice_loop.py
git commit -m "feat(ws): send real detect text from SW3" -m "修改了什么: SW3 单击改为 P0 文本测试事件，xiaozhi_ws_trigger_detect_text 构造并发送 listen detect text JSON。" -m "为什么修改: 当前实现忽略传入文本，只进入唤醒状态，无法触发服务端 TTS 闭环。" -m "影响范围: app_controller、xiaozhi_ws、P0 静态检查；目标板 ESP32-S3，SDK ESP-IDF v5.3.1，RTOS FreeRTOS。" -m "编译或测试结果: python tools/check_p0_voice_loop.py 通过；idf.py build 通过。"
```

## Task 3: Downlink Playback Sample Rate Follows Server Hello

**Files:**
- Modify: `main/bsp/audio/bsp_audio.h`
- Modify: `main/bsp/audio/bsp_audio.c`
- Modify: `main/services/audio/audio_opus_stream.c`
- Modify: `main/xiaozhi/xiaozhi_ws.c`
- Test: `tools/check_p0_voice_loop.py`

- [ ] **Step 1: Add static checks first**

In `tools/check_p0_voice_loop.py`, add:

```python
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
```

Call it from `main()` before `check_stream_direct_capture(failures)`:

```python
    check_downlink_sample_rate(failures)
```

- [ ] **Step 2: Run guardrail and verify failure**

Run:

```powershell
python tools/check_p0_voice_loop.py
```

Expected: FAIL because the sample-rate-aware BSP open path does not exist yet.

- [ ] **Step 3: Add BSP public API**

In `main/bsp/audio/bsp_audio.h`, add after `bsp_audio_open`:

```c
esp_err_t bsp_audio_open_with_sample_rate(int sample_rate);
int bsp_audio_get_current_sample_rate(void);
```

- [ ] **Step 4: Add BSP sample-rate reconfiguration**

In `main/bsp/audio/bsp_audio.c`, add this static state near `s_codec_opened`:

```c
static int s_current_sample_rate = BSP_AUDIO_SAMPLE_RATE;
```

Add these helpers after `init_i2s`:

```c
static bool is_supported_sample_rate(int sample_rate)
{
    return sample_rate == 16000 || sample_rate == 24000;
}

static esp_err_t reconfigure_i2s_sample_rate(int sample_rate)
{
    ESP_RETURN_ON_FALSE(s_i2s_tx_chan != NULL && s_i2s_rx_chan != NULL, ESP_ERR_INVALID_STATE, TAG, "I2S channels are not ready");
    ESP_RETURN_ON_FALSE(is_supported_sample_rate(sample_rate), ESP_ERR_INVALID_ARG, TAG, "unsupported audio sample rate=%d", sample_rate);

    esp_err_t tx_disable = i2s_channel_disable(s_i2s_tx_chan);
    esp_err_t rx_disable = i2s_channel_disable(s_i2s_rx_chan);
    if (tx_disable != ESP_OK && tx_disable != ESP_ERR_INVALID_STATE) {
        return tx_disable;
    }
    if (rx_disable != ESP_OK && rx_disable != ESP_ERR_INVALID_STATE) {
        return rx_disable;
    }

    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    ESP_RETURN_ON_ERROR(i2s_channel_reconfig_std_clock(s_i2s_tx_chan, &clk_cfg), TAG, "reconfig I2S TX clock failed");
    ESP_RETURN_ON_ERROR(i2s_channel_reconfig_std_clock(s_i2s_rx_chan, &clk_cfg), TAG, "reconfig I2S RX clock failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_i2s_tx_chan), TAG, "enable I2S TX failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_i2s_rx_chan), TAG, "enable I2S RX failed");

    ESP_LOGI(TAG, "I2S sample rate reconfigured to %d Hz", sample_rate);
    return ESP_OK;
}
```

- [ ] **Step 5: Implement sample-rate-aware open**

Replace `bsp_audio_open` with:

```c
esp_err_t bsp_audio_open_with_sample_rate(int sample_rate)
{
    ESP_RETURN_ON_FALSE(is_supported_sample_rate(sample_rate), ESP_ERR_INVALID_ARG, TAG, "unsupported codec sample rate=%d", sample_rate);
    ESP_RETURN_ON_ERROR(bsp_audio_init(), TAG, "init codec before open failed");

    if (s_codec_opened && s_current_sample_rate == sample_rate) {
        return ESP_OK;
    }

    if (s_codec_opened) {
        int mute_ret = esp_codec_dev_set_out_mute(s_codec, true);
        if (mute_ret != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "mute before sample-rate switch failed: %d", mute_ret);
        }
        int close_ret = esp_codec_dev_close(s_codec);
        ESP_RETURN_ON_FALSE(close_ret == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "close codec before sample-rate switch failed: %d", close_ret);
        s_codec_opened = false;
    }

    ESP_RETURN_ON_ERROR(reconfigure_i2s_sample_rate(sample_rate), TAG, "reconfigure I2S sample rate failed");

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = sample_rate,
        .channel = BSP_AUDIO_CHANNELS,
        .bits_per_sample = BSP_AUDIO_BITS_PER_SAMPLE,
    };
    int ret = esp_codec_dev_open(s_codec, &fs);
    ESP_RETURN_ON_FALSE(ret == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "open codec failed: %d", ret);

    s_codec_opened = true;
    s_current_sample_rate = sample_rate;
    ESP_LOGI(TAG, "codec opened sample_rate=%d channels=%d bits=%d", sample_rate, BSP_AUDIO_CHANNELS, BSP_AUDIO_BITS_PER_SAMPLE);
    return ESP_OK;
}

esp_err_t bsp_audio_open(void)
{
    return bsp_audio_open_with_sample_rate(BSP_AUDIO_SAMPLE_RATE);
}

int bsp_audio_get_current_sample_rate(void)
{
    return s_current_sample_rate;
}
```

- [ ] **Step 6: Open audio stream at decoder output rate**

In `main/services/audio/audio_opus_stream.c`, replace `open_audio_path` with:

```c
static esp_err_t open_audio_path(int output_volume, int playback_sample_rate)
{
    ESP_RETURN_ON_ERROR(bsp_audio_open_with_sample_rate(playback_sample_rate), TAG, "open audio codec failed");
    ESP_RETURN_ON_ERROR(bsp_audio_set_volume(output_volume), TAG, "set stream volume failed");
    ESP_RETURN_ON_FALSE(BSP_AUDIO_BITS_PER_SAMPLE == AUDIO_OPUS_BITS_PER_SAMPLE && BSP_AUDIO_CHANNELS == AUDIO_OPUS_CHANNELS,
                        ESP_ERR_NOT_SUPPORTED,
                        TAG,
                        "Opus stream expects 16-bit mono PCM");
    int mute_ret = esp_codec_dev_set_out_mute(bsp_audio_get_codec(), false);
    if (mute_ret != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "unmute stream output failed: %d", mute_ret);
    }
    ESP_LOGI(TAG, "audio path ready playback_sample_rate=%d current_codec_sample_rate=%d",
             playback_sample_rate,
             bsp_audio_get_current_sample_rate());
    return ESP_OK;
}
```

In `audio_opus_stream_start`, replace:

```c
esp_err_t err = open_audio_path(volume);
```

with:

```c
esp_err_t err = open_audio_path(volume, s_stream.decoder_output_sample_rate);
```

Also change the early-running branch so a running stream rejects sample-rate changes:

```c
    if (s_stream.running) {
        int requested_rate = config->decoder_output_sample_rate > 0 ? config->decoder_output_sample_rate : AUDIO_OPUS_SAMPLE_RATE;
        ESP_RETURN_ON_FALSE(requested_rate == s_stream.decoder_output_sample_rate,
                            ESP_ERR_INVALID_STATE,
                            TAG,
                            "cannot change running stream sample_rate old=%d new=%d",
                            s_stream.decoder_output_sample_rate,
                            requested_rate);
        s_stream.send_cb = config->send_cb;
        s_stream.user_ctx = config->user_ctx;
        return ESP_OK;
    }
```

- [ ] **Step 7: Resolve server sample rate in WebSocket**

In `main/xiaozhi/xiaozhi_ws.c`, add this helper before `start_audio_stream`:

```c
static int resolve_decoder_output_sample_rate(void)
{
    if (s_server_audio.format[0] != '\0' && strcmp(s_server_audio.format, XIAOZHI_PROTOCOL_AUDIO_FORMAT) != 0) {
        ESP_LOGW(TAG, "unsupported server audio format=%s, fallback sample_rate=%d", s_server_audio.format, AUDIO_OPUS_SAMPLE_RATE);
        return AUDIO_OPUS_SAMPLE_RATE;
    }
    if (s_server_audio.channels > 0 && s_server_audio.channels != AUDIO_OPUS_CHANNELS) {
        ESP_LOGW(TAG, "unsupported server audio channels=%d, fallback sample_rate=%d", s_server_audio.channels, AUDIO_OPUS_SAMPLE_RATE);
        return AUDIO_OPUS_SAMPLE_RATE;
    }
    if (s_server_audio.frame_duration_ms > 0 && s_server_audio.frame_duration_ms != AUDIO_OPUS_FRAME_DURATION_MS) {
        ESP_LOGW(TAG, "unsupported server frame_duration=%d, fallback sample_rate=%d", s_server_audio.frame_duration_ms, AUDIO_OPUS_SAMPLE_RATE);
        return AUDIO_OPUS_SAMPLE_RATE;
    }
    if (s_server_audio.sample_rate == 16000 || s_server_audio.sample_rate == 24000) {
        return s_server_audio.sample_rate;
    }
    ESP_LOGW(TAG, "server sample_rate=%d unsupported, fallback sample_rate=%d", s_server_audio.sample_rate, AUDIO_OPUS_SAMPLE_RATE);
    return AUDIO_OPUS_SAMPLE_RATE;
}
```

In `start_audio_stream`, replace:

```c
        .decoder_output_sample_rate = AUDIO_OPUS_SAMPLE_RATE,
```

with:

```c
        .decoder_output_sample_rate = resolve_decoder_output_sample_rate(),
```

- [ ] **Step 8: Verify**

Run:

```powershell
python tools/check_p0_voice_loop.py
idf.py build
```

Expected: guardrail PASS and firmware build PASS. Serial after server hello should include:

```text
server audio params format=opus sample_rate=24000 channels=1 frame_duration=60
audio path ready playback_sample_rate=24000 current_codec_sample_rate=24000
opus decoder ready output_sample_rate=24000 decoded_frame_bytes=2880
```

- [ ] **Step 9: Commit**

Run:

```powershell
git status
git diff -- main/bsp/audio/bsp_audio.h main/bsp/audio/bsp_audio.c main/services/audio/audio_opus_stream.c main/xiaozhi/xiaozhi_ws.c tools/check_p0_voice_loop.py
git add main/bsp/audio/bsp_audio.h main/bsp/audio/bsp_audio.c main/services/audio/audio_opus_stream.c main/xiaozhi/xiaozhi_ws.c tools/check_p0_voice_loop.py
git commit -m "feat(audio): follow server opus playback sample rate" -m "修改了什么: WebSocket server hello 的 audio_params.sample_rate 驱动 Opus decoder 与 ES8311/I2S 播放采样率。" -m "为什么修改: 服务端可能返回 24 kHz TTS，下行按 16 kHz 播放会导致速度、音调和帧长错误。" -m "影响范围: BSP audio、audio_opus_stream、xiaozhi_ws、P0 静态检查；目标板 ESP32-S3 + ES8311 + NS4150B，SDK ESP-IDF v5.3.1，RTOS FreeRTOS。" -m "编译或测试结果: python tools/check_p0_voice_loop.py 通过；idf.py build 通过。"
```

## Task 4: Explicit WebSocket Session State Machine

**Files:**
- Modify: `main/xiaozhi/xiaozhi_ws.h`
- Modify: `main/xiaozhi/xiaozhi_ws.c`
- Test: `tools/check_p0_voice_loop.py`

- [ ] **Step 1: Add static state-machine checks first**

In `tools/check_p0_voice_loop.py`, add:

```python
def check_ws_state_machine(failures: list[str]) -> None:
    header = read("main/xiaozhi/xiaozhi_ws.h")
    source = read("main/xiaozhi/xiaozhi_ws.c")

    for state in (
        "XIAOZHI_WS_STATE_CONNECTING",
        "XIAOZHI_WS_STATE_WS_CONNECTED",
        "XIAOZHI_WS_STATE_HELLO_SENT",
        "XIAOZHI_WS_STATE_READY",
        "XIAOZHI_WS_STATE_WAITING_RESPONSE",
        "XIAOZHI_WS_STATE_SPEAKING",
        "XIAOZHI_WS_STATE_CLOSING",
    ):
        require(state in header, f"xiaozhi_ws.h must define {state}", failures)

    require("set_state(XIAOZHI_WS_STATE_CONNECTING)" in source,
            "xiaozhi_ws_start must enter CONNECTING before esp_websocket_client_start", failures)
    require("set_state(XIAOZHI_WS_STATE_WS_CONNECTED)" in source,
            "websocket connected event must enter WS_CONNECTED", failures)
    require("set_state(XIAOZHI_WS_STATE_HELLO_SENT)" in source,
            "successful hello send must enter HELLO_SENT", failures)
    require("stop_session_audio_io();" in source,
            "disconnect/error paths must stop audio stream through stop_session_audio_io", failures)
```

Call it from `main()`:

```python
    check_ws_state_machine(failures)
```

- [ ] **Step 2: Run guardrail and verify failure**

Run:

```powershell
python tools/check_p0_voice_loop.py
```

Expected: FAIL because the intermediate connection states are absent.

- [ ] **Step 3: Replace public state enum**

In `main/xiaozhi/xiaozhi_ws.h`, replace the enum with:

```c
typedef enum {
    XIAOZHI_WS_STATE_DISCONNECTED = 0,
    XIAOZHI_WS_STATE_CONNECTING,
    XIAOZHI_WS_STATE_WS_CONNECTED,
    XIAOZHI_WS_STATE_HELLO_SENT,
    XIAOZHI_WS_STATE_READY,
    XIAOZHI_WS_STATE_WAKE_DETECTED,
    XIAOZHI_WS_STATE_LISTENING,
    XIAOZHI_WS_STATE_WAITING_RESPONSE,
    XIAOZHI_WS_STATE_SPEAKING,
    XIAOZHI_WS_STATE_CLOSING,
} xiaozhi_ws_state_t;
```

- [ ] **Step 4: Update state names and state predicates**

In `main/xiaozhi/xiaozhi_ws.c`, update `state_name` cases:

```c
    case XIAOZHI_WS_STATE_CONNECTING:
        return "CONNECTING";
    case XIAOZHI_WS_STATE_WS_CONNECTED:
        return "WS_CONNECTED";
    case XIAOZHI_WS_STATE_HELLO_SENT:
        return "HELLO_SENT";
    case XIAOZHI_WS_STATE_CLOSING:
        return "CLOSING";
```

Remove any `XIAOZHI_WS_STATE_RECONNECTING` references.

Add these helpers after `reset_session_flags`:

```c
static bool is_ready_or_busy_state(xiaozhi_ws_state_t state)
{
    return state == XIAOZHI_WS_STATE_READY ||
           state == XIAOZHI_WS_STATE_WAKE_DETECTED ||
           state == XIAOZHI_WS_STATE_LISTENING ||
           state == XIAOZHI_WS_STATE_WAITING_RESPONSE ||
           state == XIAOZHI_WS_STATE_SPEAKING;
}

static bool can_send_business_message(void)
{
    return s_ws_client != NULL &&
           esp_websocket_client_is_connected(s_ws_client) &&
           (s_ws_state == XIAOZHI_WS_STATE_READY ||
            s_ws_state == XIAOZHI_WS_STATE_WAKE_DETECTED ||
            s_ws_state == XIAOZHI_WS_STATE_WAITING_RESPONSE);
}
```

Use `is_ready_or_busy_state` in `xiaozhi_ws_is_ready`:

```c
bool xiaozhi_ws_is_ready(void)
{
    return is_ready_or_busy_state(s_ws_state) && s_ws_state != XIAOZHI_WS_STATE_SPEAKING;
}
```

- [ ] **Step 5: Update connected and hello states**

In `send_hello`, after `send_text_json(json, "hello")` succeeds:

```c
    if (err == ESP_OK) {
        set_state(XIAOZHI_WS_STATE_HELLO_SENT);
        log_heap_stats("hello sent");
    }
```

In `websocket_event_handler`, replace the connected case with:

```c
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "websocket connected");
        set_state(XIAOZHI_WS_STATE_WS_CONNECTED);
        if (send_hello() != ESP_OK) {
            set_state(XIAOZHI_WS_STATE_DISCONNECTED);
        }
        break;
```

In `handle_server_hello`, keep `set_state(XIAOZHI_WS_STATE_READY)` after audio stream startup succeeds.

- [ ] **Step 6: Update start/stop transitions**

In `xiaozhi_ws_start`, replace the existing "already started" state list with:

```c
        if (s_ws_state == XIAOZHI_WS_STATE_CONNECTING ||
            s_ws_state == XIAOZHI_WS_STATE_WS_CONNECTED ||
            s_ws_state == XIAOZHI_WS_STATE_HELLO_SENT ||
            is_ready_or_busy_state(s_ws_state)) {
            ESP_LOGW(TAG, "websocket client already started state=%s", state_name(s_ws_state));
            return ESP_OK;
        }
```

Before `esp_websocket_client_start(s_ws_client)`, replace the reconnecting transition with:

```c
    set_state(XIAOZHI_WS_STATE_CONNECTING);
```

In `xiaozhi_ws_stop`, set closing before cleanup:

```c
    set_state(XIAOZHI_WS_STATE_CLOSING);
    stop_opus_audio_stream();
    cleanup_websocket_client();
```

- [ ] **Step 7: Use the business-message predicate**

In `xiaozhi_ws_trigger_detect_text`, after `ensure_websocket_ready()` succeeds, replace the state check with:

```c
    if (!can_send_business_message()) {
        ESP_LOGW(TAG, "detect text invalid state=%s connected=%d",
                 state_name(s_ws_state),
                 s_ws_client != NULL ? esp_websocket_client_is_connected(s_ws_client) : 0);
        return ESP_ERR_INVALID_STATE;
    }
```

In `send_listen_state`, add before JSON allocation:

```c
    ESP_RETURN_ON_FALSE(can_send_business_message() || s_ws_state == XIAOZHI_WS_STATE_LISTENING,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "listen state=%s cannot be sent in ws state=%s",
                        state,
                        state_name(s_ws_state));
```

- [ ] **Step 8: Verify**

Run:

```powershell
python tools/check_p0_voice_loop.py
idf.py build
```

Expected serial sequence:

```text
state transition DISCONNECTED -> CONNECTING
state transition CONNECTING -> WS_CONNECTED
hello sent
state transition WS_CONNECTED -> HELLO_SENT
server hello received
state transition HELLO_SENT -> READY
```

- [ ] **Step 9: Commit**

Run:

```powershell
git status
git diff -- main/xiaozhi/xiaozhi_ws.h main/xiaozhi/xiaozhi_ws.c tools/check_p0_voice_loop.py
git add main/xiaozhi/xiaozhi_ws.h main/xiaozhi/xiaozhi_ws.c tools/check_p0_voice_loop.py
git commit -m "feat(ws): make session state progression explicit" -m "修改了什么: WebSocket 状态从连接、hello、READY、业务等待、播放、关闭分阶段表达，并集中约束业务消息发送时机。" -m "为什么修改: 按键、重连、hello、listen、音频任务之间需要严格顺序，否则 P0 闭环在断线和重连后不可预测。" -m "影响范围: xiaozhi_ws 状态机与 P0 静态检查；目标板 ESP32-S3，SDK ESP-IDF v5.3.1，RTOS FreeRTOS。" -m "编译或测试结果: python tools/check_p0_voice_loop.py 通过；idf.py build 通过。"
```

## Task 5: Standardized Heap and Stack Watermarks

**Files:**
- Modify: `main/services/audio/audio_opus_stream.h`
- Modify: `main/services/audio/audio_opus_stream.c`
- Modify: `main/xiaozhi/xiaozhi_ws.c`
- Test: `tools/check_p0_voice_loop.py`

- [ ] **Step 1: Add static diagnostics checks first**

In `tools/check_p0_voice_loop.py`, add:

```python
def check_runtime_diagnostics(failures: list[str]) -> None:
    stream_header = read("main/services/audio/audio_opus_stream.h")
    stream_source = read("main/services/audio/audio_opus_stream.c")
    ws_source = read("main/xiaozhi/xiaozhi_ws.c")

    require("audio_opus_stream_log_watermarks" in stream_header,
            "audio_opus_stream.h must expose audio_opus_stream_log_watermarks", failures)
    require("uxTaskGetStackHighWaterMark" in stream_source,
            "audio_opus_stream.c must log task stack watermarks", failures)
    for label in (
        "WS READY",
        "Opus stream started",
        "TTS start",
        "TTS stop",
        "binary opus",
    ):
        require(label in ws_source, f"xiaozhi_ws.c must log diagnostics label {label}", failures)
```

Call it from `main()`:

```python
    check_runtime_diagnostics(failures)
```

- [ ] **Step 2: Run guardrail and verify failure**

Run:

```powershell
python tools/check_p0_voice_loop.py
```

Expected: FAIL because the stream watermark API and fixed labels are absent.

- [ ] **Step 3: Expose diagnostics API**

In `main/services/audio/audio_opus_stream.h`, add:

```c
void audio_opus_stream_log_watermarks(const char *label);
```

- [ ] **Step 4: Implement diagnostics API**

In `main/services/audio/audio_opus_stream.c`, add this function before `audio_opus_stream_start`:

```c
static unsigned int task_watermark(TaskHandle_t task)
{
    return task != NULL ? (unsigned int)uxTaskGetStackHighWaterMark(task) : 0;
}

void audio_opus_stream_log_watermarks(const char *label)
{
    const char *name = (label != NULL && label[0] != '\0') ? label : "audio_opus_stream";
    ESP_LOGI(TAG,
             "%s runtime: running=%d uplink=%d pending_downlink=%u tx_frames=%u rx_frames=%u decoded_frames=%u playback_failures=%u uplink_drops=%u downlink_drops=%u internal_free=%u internal_largest=%u spiram_free=%u spiram_largest=%u encoder_stack=%u decoder_stack=%u capture_stack=%u",
             name,
             s_stream.running,
             s_stream.uplink_enabled,
             (unsigned int)s_stream.downlink_pending_frames,
             (unsigned int)s_stream.tx_frames,
             (unsigned int)s_stream.rx_frames,
             (unsigned int)s_stream.decoded_frames,
             (unsigned int)s_stream.playback_failures,
             (unsigned int)s_stream.uplink_drop_count,
             (unsigned int)s_stream.downlink_drop_count,
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             task_watermark(s_stream.encoder_task),
             task_watermark(s_stream.decoder_task),
             task_watermark(s_stream.capture_task));
}
```

At the end of `audio_opus_stream_start`, before `return ESP_OK`, add:

```c
    audio_opus_stream_log_watermarks("Opus stream started");
```

- [ ] **Step 5: Call diagnostics from WebSocket lifecycle**

In `handle_server_hello`, after `set_state(XIAOZHI_WS_STATE_READY);`, add:

```c
    log_heap_stats("WS READY");
    audio_opus_stream_log_watermarks("WS READY");
```

In `handle_tts`, inside the `start` branch after `set_state(XIAOZHI_WS_STATE_SPEAKING);`, add:

```c
        log_heap_stats("TTS start");
        audio_opus_stream_log_watermarks("TTS start");
```

Inside the `stop` branch before `return;`, add:

```c
        log_heap_stats("TTS stop");
        audio_opus_stream_log_watermarks("TTS stop");
```

In `handle_binary_opus`, after a successful enqueue:

```c
    if (err == ESP_OK) {
        audio_opus_stream_log_watermarks("binary opus");
    }
```

- [ ] **Step 6: Verify**

Run:

```powershell
python tools/check_p0_voice_loop.py
idf.py build
```

Expected serial checkpoints:

```text
WS READY heap: internal_free=...
WS READY runtime: running=1 ...
TTS start heap: internal_free=...
binary opus runtime: running=1 ...
TTS stop runtime: running=1 ...
```

- [ ] **Step 7: Commit**

Run:

```powershell
git status
git diff -- main/services/audio/audio_opus_stream.h main/services/audio/audio_opus_stream.c main/xiaozhi/xiaozhi_ws.c tools/check_p0_voice_loop.py
git add main/services/audio/audio_opus_stream.h main/services/audio/audio_opus_stream.c main/xiaozhi/xiaozhi_ws.c tools/check_p0_voice_loop.py
git commit -m "feat(audio): standardize voice loop diagnostics" -m "修改了什么: 增加固定生命周期 heap 与音频任务 stack watermark 日志。" -m "为什么修改: P0 验收需要在 READY、Opus start、TTS 播放中和 stop 后观察内部堆、PSRAM、任务栈水位。" -m "影响范围: audio_opus_stream、xiaozhi_ws、P0 静态检查；目标板 ESP32-S3，SDK ESP-IDF v5.3.1，RTOS FreeRTOS。" -m "编译或测试结果: python tools/check_p0_voice_loop.py 通过；idf.py build 通过。"
```

## Task 6: End-to-End P0 Verification

**Files:**
- No source edits expected in this task.

- [ ] **Step 1: Full clean build**

Run:

```powershell
idf.py fullclean
idf.py build
```

Expected: firmware build PASS.

- [ ] **Step 2: Flash and monitor**

Run:

```powershell
idf.py flash monitor
```

Expected after Wi-Fi/OTA activation:

```text
websocket headers configured Device-Id=... Client-Id=...
state transition DISCONNECTED -> CONNECTING
state transition CONNECTING -> WS_CONNECTED
hello sent
state transition WS_CONNECTED -> HELLO_SENT
server hello received
server audio params format=opus sample_rate=24000 channels=1 frame_duration=60
state transition HELLO_SENT -> READY
WS READY heap: internal_free=...
```

- [ ] **Step 3: Press SW3 once**

Expected:

```text
SW3 P0 text test trigger detected
listen detect payload={"session_id":"...","type":"listen","state":"detect","text":"你好，请介绍你自己"}
listen detect sent
state transition READY -> WAITING_RESPONSE
tts state=start
state transition WAITING_RESPONSE -> SPEAKING
binary opus received len=...
downlink opus queued frames=...
decoded pcm bytes=2880 decoded_frames=...
speaker playback OK
tts state=stop
state transition SPEAKING -> READY
```

- [ ] **Step 4: Idle disconnect recovery**

Wait at least 70 seconds after READY. If the service closes the socket, press SW3 once again.

Expected:

```text
websocket disconnected
state transition ... -> DISCONNECTED
SW3 P0 text test trigger detected
state transition DISCONNECTED -> CONNECTING
state transition CONNECTING -> WS_CONNECTED
state transition WS_CONNECTED -> HELLO_SENT
state transition HELLO_SENT -> READY
listen detect payload=...
```

- [ ] **Step 5: Capture acceptance notes**

Append one paragraph to `docs/xiaozhi_voice_loop_progress_report.md` with these exact fields:

```markdown
## P0 Text Voice Loop Verification - 2026-05-16

- Build command:
- Flash/monitor command:
- Server hello sample_rate:
- SW3 detect text sent:
- TTS JSON observed:
- Binary OPUS frames observed:
- Speaker playback result:
- READY idle disconnect recovery:
- Internal heap low watermark:
- PSRAM free after TTS:
- Audio task stack watermark:
```

Fill each field with the observed value from the monitor log. If hardware playback is silent while binary frames decode, write `Speaker playback result: silent; decoder path OK; ES8311/I2S/PA requires audio-board isolation`.

- [ ] **Step 6: Commit verification notes**

Run:

```powershell
git status
git diff -- docs/xiaozhi_voice_loop_progress_report.md
git add docs/xiaozhi_voice_loop_progress_report.md
git commit -m "docs(voice): record P0 text loop verification" -m "修改了什么: 记录 P0 文本触发、TTS 下行、OPUS 解码、播放和断线恢复验收结果。" -m "为什么修改: 需要把真实板级验证结果固定下来，避免继续叠加 SR/VAD 前缺少证据。" -m "影响范围: 文档；目标板 ESP32-S3 + ES8311 + NS4150B，SDK ESP-IDF v5.3.1，RTOS FreeRTOS。" -m "编译或测试结果: idf.py build 与板级 monitor 结果见文档。"
```

## Acceptance Gate

This plan is complete only when all of these are true:

- `python tools/check_p0_voice_loop.py` passes.
- `idf.py -C test_apps/xiaozhi_protocol_test build` passes.
- `idf.py build` passes.
- SW3 single click emits a `listen detect` payload containing `text`.
- Server hello 24 kHz causes decoder and codec playback to use 24 kHz.
- Monitor shows at least one binary OPUS frame received and at least one decoded PCM frame written to speaker path.
- READY idle disconnect does not break the next SW3 text request.
- `CONFIG_XIAOZHI_STAGE1_AUTO_SR_ENABLE` remains disabled.

## References

- Espressif XiaoZhi component overview: https://docs.espressif.com/projects/esp-iot-solution/zh_CN/latest/ai/xiaozhi.html
- XiaoZhi WebSocket protocol: https://xiaozhi.me/xz-docs/docs/tutorial-comm/websocket-comm/
