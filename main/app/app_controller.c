#include "app_controller.h"

#include "app_input_controller.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "provisioning_screen.h"
#include "provisioning_service.h"
#include "status_led_service.h"
#include "xiaozhi_stage1.h"
#include "xiaozhi_ui.h"
#include "xiaozhi_ws.h"

static const char *TAG = "app_controller";

#define VOICE_SESSION_TASK_STACK 4096
#define VOICE_SESSION_TASK_PRIORITY 5
#define VOICE_SESSION_QUEUE_LEN 4
#define XIAOZHI_P0_TEXT_TEST "你好，请介绍你自己"

typedef enum {
    VOICE_SESSION_EVT_TEXT_TEST = 0,
    VOICE_SESSION_EVT_LISTEN_START,
    VOICE_SESSION_EVT_LISTEN_STOP,
} voice_session_evt_t;

static QueueHandle_t s_voice_evt_queue;
static TaskHandle_t s_voice_session_task;

#define UI_TEXT_XIAOZHI_TITLE "尚硅谷" "AI" "小智"
#define UI_TEXT_START_PROV "正在启动扫码配网"
#define UI_TEXT_SCAN_PROV "请扫码配网!"
#define UI_TEXT_MAKE_QR "正在生成配网二维码"
#define UI_TEXT_WIFI_GOT "收到WiFi信息"
#define UI_TEXT_WIFI_JOINING "正在连接无线网络"
#define UI_TEXT_WIFI_CONNECTING "正在连接WiFi"
#define UI_TEXT_KEEP_POWER "请保持设备通电"
#define UI_TEXT_WIFI_CONNECTED "WiFi已连接"
#define UI_TEXT_START_XIAOZHI "正在启动小智"
#define UI_TEXT_PROV_FAILED "配网失败"
#define UI_TEXT_CHECK_WIFI "请检查WiFi密码，稍后重新扫码"
#define UI_TEXT_CONNECTING_XIAOZHI "正在连接小智"
#define UI_TEXT_NET_READY "网络已就绪"

static void business_start_cb(void *user_ctx);
static void provisioning_state_cb(provisioning_service_state_t state, void *user_ctx);
static void provisioning_qrcode_cb(const char *payload, void *user_ctx);
static esp_err_t reset_provisioning_cb(void *user_ctx);
static esp_err_t voice_trigger_cb(void *user_ctx);
static esp_err_t voice_event_cb(app_input_voice_evt_t input_evt, void *user_ctx);
static void voice_session_task(void *arg);

esp_err_t app_controller_start(void)
{
    const app_input_controller_config_t input_config = {
        .reset_provisioning_cb = reset_provisioning_cb,
        .voice_trigger_cb = voice_trigger_cb,
        .voice_event_cb = voice_event_cb,
        .user_ctx = NULL,
    };
    ESP_RETURN_ON_ERROR(app_input_controller_init(&input_config), TAG, "init input controller failed");

    const provisioning_service_config_t provisioning_config = {
        .business_start_cb = business_start_cb,
        .state_cb = provisioning_state_cb,
        .qrcode_cb = provisioning_qrcode_cb,
        .user_ctx = NULL,
    };

    ESP_RETURN_ON_ERROR(provisioning_service_start(&provisioning_config), TAG, "start provisioning service failed");
    return ESP_OK;
}

static void business_start_cb(void *user_ctx)
{
    (void)user_ctx;

    esp_err_t display_err = xiaozhi_ui_init();
    if (display_err != ESP_OK) {
        ESP_LOGE(TAG, "display init failed after Wi-Fi connected: %s", esp_err_to_name(display_err));
    } else {
        xiaozhi_ui_show_ota_loading();
    }

    esp_err_t led_err = status_led_service_start();
    if (led_err != ESP_OK) {
        ESP_LOGE(TAG, "start status led failed: %s", esp_err_to_name(led_err));
    }

    esp_err_t xiaozhi_err = xiaozhi_stage1_start();
    if (xiaozhi_err != ESP_OK) {
        ESP_LOGE(TAG, "start xiaozhi stage1 failed: %s", esp_err_to_name(xiaozhi_err));
    }
}

static void provisioning_state_cb(provisioning_service_state_t state, void *user_ctx)
{
    (void)user_ctx;
    ESP_LOGI(TAG, "provisioning state changed: %d", state);

    switch (state) {
    case PROVISIONING_SERVICE_STATE_UNPROVISIONED:
        provisioning_screen_show_status(UI_TEXT_XIAOZHI_TITLE, UI_TEXT_START_PROV);
        break;
    case PROVISIONING_SERVICE_STATE_PROVISIONING:
        provisioning_screen_show_status(UI_TEXT_SCAN_PROV, UI_TEXT_MAKE_QR);
        break;
    case PROVISIONING_SERVICE_STATE_CRED_RECEIVED:
        provisioning_screen_show_status(UI_TEXT_WIFI_GOT, UI_TEXT_WIFI_JOINING);
        break;
    case PROVISIONING_SERVICE_STATE_CONNECTING:
        provisioning_screen_show_status(UI_TEXT_WIFI_CONNECTING, UI_TEXT_KEEP_POWER);
        break;
    case PROVISIONING_SERVICE_STATE_CONNECTED:
        provisioning_screen_show_status(UI_TEXT_WIFI_CONNECTED, UI_TEXT_START_XIAOZHI);
        break;
    case PROVISIONING_SERVICE_STATE_FAILED:
        provisioning_screen_show_status(UI_TEXT_PROV_FAILED, UI_TEXT_CHECK_WIFI);
        break;
    case PROVISIONING_SERVICE_STATE_BUSINESS_STARTED:
        provisioning_screen_show_status(UI_TEXT_CONNECTING_XIAOZHI, UI_TEXT_NET_READY);
        break;
    default:
        break;
    }
}

static void provisioning_qrcode_cb(const char *payload, void *user_ctx)
{
    (void)user_ctx;
    provisioning_screen_show_qrcode(payload);
}

static esp_err_t reset_provisioning_cb(void *user_ctx)
{
    (void)user_ctx;
    return provisioning_service_reset_and_restart();
}

static esp_err_t voice_trigger_cb(void *user_ctx)
{
    (void)user_ctx;
    ESP_LOGI(TAG, "button voice text test queued");
    if (s_voice_evt_queue == NULL) {
        ESP_LOGW(TAG, "voice trigger ignored before voice session is ready");
        return ESP_OK;
    }

    const voice_session_evt_t evt = VOICE_SESSION_EVT_TEXT_TEST;
    BaseType_t sent = xQueueSend(s_voice_evt_queue, &evt, 0);
    return sent == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

static esp_err_t voice_event_cb(app_input_voice_evt_t input_evt, void *user_ctx)
{
    (void)user_ctx;
    voice_session_evt_t evt;

    switch (input_evt) {
    case APP_INPUT_VOICE_EVT_TEXT_TEST:
        evt = VOICE_SESSION_EVT_TEXT_TEST;
        break;
    case APP_INPUT_VOICE_EVT_LISTEN_START:
        evt = VOICE_SESSION_EVT_LISTEN_START;
        break;
    case APP_INPUT_VOICE_EVT_LISTEN_STOP:
        evt = VOICE_SESSION_EVT_LISTEN_STOP;
        break;
    default:
        ESP_LOGW(TAG, "unknown input voice event=%d", input_evt);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "button voice event queued=%d", evt);
    if (s_voice_evt_queue == NULL) {
        ESP_LOGW(TAG, "voice event ignored before voice session is ready");
        return ESP_OK;
    }

    BaseType_t sent = xQueueSend(s_voice_evt_queue, &evt, 0);
    return sent == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t app_controller_start_voice_session(void)
{
    if (s_voice_evt_queue == NULL) {
        s_voice_evt_queue = xQueueCreate(VOICE_SESSION_QUEUE_LEN, sizeof(voice_session_evt_t));
        ESP_RETURN_ON_FALSE(s_voice_evt_queue != NULL, ESP_ERR_NO_MEM, TAG, "create voice event queue failed");
    }

    if (s_voice_session_task != NULL) {
        return ESP_OK;
    }

    BaseType_t created = xTaskCreate(voice_session_task,
                                     "voice_session",
                                     VOICE_SESSION_TASK_STACK,
                                     NULL,
                                     VOICE_SESSION_TASK_PRIORITY,
                                     &s_voice_session_task);
    ESP_RETURN_ON_FALSE(created == pdPASS, ESP_ERR_NO_MEM, TAG, "create voice session task failed");
    ESP_LOGI(TAG, "voice session task started");
    return ESP_OK;
}

static void voice_session_task(void *arg)
{
    (void)arg;
    voice_session_evt_t evt;

    while (true) {
        if (xQueueReceive(s_voice_evt_queue, &evt, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        switch (evt) {
        case VOICE_SESSION_EVT_TEXT_TEST: {
            ESP_LOGI(TAG, "SW3 P0 text test trigger detected");
            esp_err_t err = xiaozhi_ws_trigger_detect_text(XIAOZHI_P0_TEXT_TEST);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "button detect ignored: %s", esp_err_to_name(err));
            }
            break;
        }
        case VOICE_SESSION_EVT_LISTEN_START: {
            ESP_LOGI(TAG, "SW3 manual listen start event");
            esp_err_t err = xiaozhi_ws_trigger_listen(XIAOZHI_WS_LISTEN_MODE_BUTTON);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "manual listen start ignored: %s", esp_err_to_name(err));
            }
            break;
        }
        case VOICE_SESSION_EVT_LISTEN_STOP: {
            ESP_LOGI(TAG, "SW3 manual listen stop event");
            esp_err_t err = xiaozhi_ws_stop_listen();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "manual listen stop failed: %s", esp_err_to_name(err));
            }
            break;
        }
        default:
            ESP_LOGW(TAG, "unknown voice event=%d", evt);
            break;
        }
    }
}
