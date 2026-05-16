#include "xiaozhi_stage1.h"
#include "app_controller.h"
#include "audio_opus_stream.h"
#include "xiaozhi_ws.h"


#include <stdbool.h>

#include <string.h>



#include "esp_log.h"

#include "freertos/FreeRTOS.h"

#include "freertos/task.h"

#include "sdkconfig.h"

#include "xiaozhi_device.h"

#include "xiaozhi_handle.h"

#include "xiaozhi_ota.h"

#include "xiaozhi_ui.h"

#include "xiaozhi_sr.h"


static const char *TAG = "xiaozhi_stage1";



#define XIAOZHI_STAGE1_OTA_TASK_STACK 6144

#define XIAOZHI_STAGE1_OTA_TASK_PRIORITY 5

#define XIAOZHI_STAGE1_ACTIVATION_POLL_MS 3000
#define XIAOZHI_STAGE1_WS_READY_TIMEOUT_MS 15000
#define XIAOZHI_STAGE1_WS_READY_POLL_MS 100
#define UI_TEXT_CONNECT_FAILED "连接失败"

#define UI_TEXT_INIT_FAILED "设备初始化失败"

#define UI_TEXT_CHECK_NETWORK "请检查网络或服务器"

#define UI_TEXT_TASK_CREATE_FAILED "OTA任务创建失败"

#define UI_TEXT_AI_START_FAILED "AI启动失败"



static TaskHandle_t s_ota_task_handle;

static bool s_ota_task_starting;

static portMUX_TYPE s_ota_task_lock = portMUX_INITIALIZER_UNLOCKED;



static void log_token_len(void);
static esp_err_t wait_for_websocket_ready(void);
#if CONFIG_XIAOZHI_STAGE1_AUTO_SR_ENABLE
static void sr_wake_cb(void *user_ctx);
static void sr_vad_state_cb(vad_state_t state, void *user_ctx);
static void sr_pcm_output_cb(const uint8_t *data, size_t len, void *user_ctx);
#endif


static void log_activation_snapshot(void)

{

    const char *activation_code = xiaozhi_handle_get_activation_code();



    ESP_LOGI(TAG, "activated=%s", xiaozhi_handle_is_activated() ? "true" : "false");

    ESP_LOGI(TAG, "activation code=%s", activation_code != NULL ? activation_code : "<none>");

    ESP_LOGI(TAG, "websocket url present=%s", xiaozhi_handle_get_websocket_url() != NULL ? "yes" : "no");

    log_token_len();

}



static void wait_for_activation(const xiaozhi_ota_config_t *ota_config)

{

    while (!xiaozhi_handle_is_activated()) {

        const char *activation_code = xiaozhi_handle_get_activation_code();

        xiaozhi_ui_show_activation_required(activation_code);

        ESP_LOGI(TAG, "waiting for console activation, retry ota in %d ms", XIAOZHI_STAGE1_ACTIVATION_POLL_MS);



        vTaskDelay(pdMS_TO_TICKS(XIAOZHI_STAGE1_ACTIVATION_POLL_MS));



        esp_err_t err = xiaozhi_ota_request(ota_config);

        if (err != ESP_OK) {

            ESP_LOGW(TAG, "activation polling ota request failed: %s", esp_err_to_name(err));

            continue;

        }



        log_activation_snapshot();

    }

}



static esp_err_t enter_ai_after_activation(void)

{

    xiaozhi_ui_show_welcome();



    esp_err_t err = xiaozhi_ws_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start websocket failed: %s", esp_err_to_name(err));
        xiaozhi_ui_show_error(UI_TEXT_CONNECT_FAILED, UI_TEXT_AI_START_FAILED);
        return err;
    }

    ESP_LOGI(TAG, "waiting for websocket READY before SR gate");
    err = wait_for_websocket_ready();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "websocket ready wait failed: %s", esp_err_to_name(err));
        (void)xiaozhi_ws_stop();
        xiaozhi_ui_show_error(UI_TEXT_CONNECT_FAILED, UI_TEXT_AI_START_FAILED);
        return err;
    }

    err = app_controller_start_voice_session();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start voice session task after websocket READY failed: %s", esp_err_to_name(err));
        xiaozhi_ui_show_error(UI_TEXT_CONNECT_FAILED, UI_TEXT_AI_START_FAILED);
        return err;
    }

#if !CONFIG_XIAOZHI_STAGE1_AUTO_SR_ENABLE
    ESP_LOGI(TAG, "websocket READY; SR auto init disabled");
    return ESP_OK;
}
#else
    const xiaozhi_sr_callbacks_t sr_callbacks = {
        .vad_state_cb = sr_vad_state_cb,
        .wake_cb = sr_wake_cb,
        .pcm_output_cb = sr_pcm_output_cb,
        .user_ctx = NULL,
    };

    err = xiaozhi_sr_init(&sr_callbacks);
    if (err != ESP_OK) {

        ESP_LOGE(TAG, "start xiaozhi sr failed: %s", esp_err_to_name(err));
        xiaozhi_ui_show_error(UI_TEXT_CONNECT_FAILED, UI_TEXT_AI_START_FAILED);
        return err;

    }



    return ESP_OK;

}



#endif

static void log_token_len(void)
{
    const char *token = xiaozhi_handle_get_websocket_token();

    ESP_LOGI(TAG, "websocket token length=%u", token != NULL ? (unsigned int)strlen(token) : 0U);

}



static esp_err_t wait_for_websocket_ready(void)
{
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(XIAOZHI_STAGE1_WS_READY_TIMEOUT_MS);

    while (xTaskGetTickCount() < deadline) {
        if (xiaozhi_ws_is_ready()) {
            return ESP_OK;
        }

        xiaozhi_ws_state_t state = xiaozhi_ws_get_state();
        if (state == XIAOZHI_WS_STATE_DISCONNECTED) {
            ESP_LOGE(TAG, "websocket failed before READY state=%d", state);
            return ESP_FAIL;
        }

        vTaskDelay(pdMS_TO_TICKS(XIAOZHI_STAGE1_WS_READY_POLL_MS));
    }

    ESP_LOGE(TAG, "websocket READY wait timeout after %d ms", XIAOZHI_STAGE1_WS_READY_TIMEOUT_MS);
    return ESP_ERR_TIMEOUT;
}

#if CONFIG_XIAOZHI_STAGE1_AUTO_SR_ENABLE
static void sr_wake_cb(void *user_ctx)
{
    (void)user_ctx;
    ESP_LOGI(TAG, "wake word detected");
    esp_err_t err = xiaozhi_ws_on_wake_detected();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wake listen start ignored: %s", esp_err_to_name(err));
    }
}

static void sr_vad_state_cb(vad_state_t state, void *user_ctx)
{
    (void)user_ctx;
    if (state == VAD_SPEECH) {
        ESP_LOGI(TAG, "vad speech");
        esp_err_t err = xiaozhi_ws_on_vad_state(true);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "vad speech handling failed: %s", esp_err_to_name(err));
        }
        return;
    }

    ESP_LOGI(TAG, "vad silence");
    if (xiaozhi_ws_get_state() == XIAOZHI_WS_STATE_LISTENING) {
        esp_err_t err = xiaozhi_ws_on_vad_state(false);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "listen stop on VAD silence failed: %s", esp_err_to_name(err));
        }
    }
}

static void sr_pcm_output_cb(const uint8_t *data, size_t len, void *user_ctx)
{
    (void)user_ctx;
    esp_err_t err = xiaozhi_ws_feed_processed_pcm(data, len);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE && err != ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "feed opus uplink pcm failed: %s", esp_err_to_name(err));
    }
}
#endif

static void log_device_snapshot(void)
{

    char uuid[XIAOZHI_UUID_STR_LEN] = {0};

    char mac[XIAOZHI_MAC_STR_LEN] = {0};

    char ip[XIAOZHI_IPV4_STR_LEN] = {0};

    char ssid[33] = {0};

    int rssi = 0;

    int channel = 0;



    esp_err_t err = xiaozhi_device_get_or_create_uuid(uuid, sizeof(uuid));

    if (err == ESP_OK) {

        ESP_LOGI(TAG, "UUID=%s", uuid);

    } else {

        ESP_LOGW(TAG, "get UUID failed: %s", esp_err_to_name(err));

    }



    err = xiaozhi_device_get_mac_str(mac, sizeof(mac));

    if (err == ESP_OK) {

        ESP_LOGI(TAG, "MAC=%s", mac);

    } else {

        ESP_LOGW(TAG, "get MAC failed: %s", esp_err_to_name(err));

    }



    err = xiaozhi_device_get_ip_str(ip, sizeof(ip));

    if (err == ESP_OK) {

        ESP_LOGI(TAG, "IP=%s", ip);

    } else {

        ESP_LOGW(TAG, "get IP failed: %s", esp_err_to_name(err));

    }



    err = xiaozhi_device_get_wifi_info(ssid, sizeof(ssid), &rssi, &channel);

    if (err == ESP_OK) {

        ESP_LOGI(TAG, "WiFi ssid=%s rssi=%d channel=%d", ssid, rssi, channel);

    } else {

        ESP_LOGW(TAG, "get WiFi info failed: %s", esp_err_to_name(err));

    }

}



static void ota_task(void *arg)

{

    (void)arg;



    esp_err_t err = xiaozhi_handle_init();

    if (err != ESP_OK) {

        ESP_LOGE(TAG, "init xiaozhi handle failed: %s", esp_err_to_name(err));

        xiaozhi_ui_show_error(UI_TEXT_CONNECT_FAILED, UI_TEXT_INIT_FAILED);

        goto cleanup;

    }



    xiaozhi_handle_clear_runtime();

    xiaozhi_ui_show_ota_loading();

    log_device_snapshot();



    const xiaozhi_ota_config_t ota_config = {

        .ota_url = XIAOZHI_DEFAULT_OTA_URL,

        .timeout_ms = CONFIG_XIAOZHI_HTTP_TIMEOUT_MS,

    };



    err = xiaozhi_ota_request(&ota_config);

    if (err != ESP_OK) {

        ESP_LOGE(TAG, "xiaozhi ota request failed: %s", esp_err_to_name(err));

        xiaozhi_ui_show_error(UI_TEXT_CONNECT_FAILED, UI_TEXT_CHECK_NETWORK);

        goto cleanup;

    }



    bool activated = xiaozhi_handle_is_activated();

    log_activation_snapshot();



    if (!activated) {

        wait_for_activation(&ota_config);

    }



    err = enter_ai_after_activation();

    if (err != ESP_OK) {

        goto cleanup;

    }



cleanup:

    taskENTER_CRITICAL(&s_ota_task_lock);

    s_ota_task_handle = NULL;

    s_ota_task_starting = false;

    taskEXIT_CRITICAL(&s_ota_task_lock);

    vTaskDelete(NULL);

}



esp_err_t xiaozhi_stage1_start(void)

{

    taskENTER_CRITICAL(&s_ota_task_lock);

    bool already_running = s_ota_task_starting || s_ota_task_handle != NULL;

    if (!already_running) {

        s_ota_task_starting = true;

    }

    taskEXIT_CRITICAL(&s_ota_task_lock);



    if (already_running) {

        ESP_LOGW(TAG, "xiaozhi ota task is already running");

        return ESP_OK;

    }



    BaseType_t created = xTaskCreate(ota_task,

                                     "xiaozhi_ota",

                                     XIAOZHI_STAGE1_OTA_TASK_STACK,

                                     NULL,

                                     XIAOZHI_STAGE1_OTA_TASK_PRIORITY,

                                     &s_ota_task_handle);



    taskENTER_CRITICAL(&s_ota_task_lock);

    s_ota_task_starting = false;

    if (created != pdPASS) {

        s_ota_task_handle = NULL;

    }

    taskEXIT_CRITICAL(&s_ota_task_lock);



    if (created != pdPASS) {

        ESP_LOGE(TAG, "create xiaozhi ota task failed");

        xiaozhi_ui_show_error(UI_TEXT_CONNECT_FAILED, UI_TEXT_TASK_CREATE_FAILED);

        return ESP_ERR_NO_MEM;

    }



    ESP_LOGI(TAG, "xiaozhi ota task started");

    return ESP_OK;

}

