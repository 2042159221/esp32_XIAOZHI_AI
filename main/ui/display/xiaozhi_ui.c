#include "xiaozhi_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp_lcd.h"
#include "display_service.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "font_emoji.h"
#include "lvgl.h"

static const char *TAG = "xiaozhi_ui";

#define XIAOZHI_UI_QRCODE_SIZE 176
#define XIAOZHI_UI_SAFE_TEXT(value) ((value) != NULL ? (value) : "")

#define UI_TEXT_TITLE "浮浮酱~"
#define UI_TEXT_HELLO "hi ~ ，我是浮浮酱~"
#define UI_TEXT_PROV_TITLE "请扫码配网!"
#define UI_TEXT_PROV_HINT "请使用手机扫码完成 WiFi 配网"
#define UI_TEXT_PROV_FAILED "配网失败"
#define UI_TEXT_QR_EMPTY "二维码内容为空"
#define UI_TEXT_QR_GEN_FAILED "二维码生成失败"
#define UI_TEXT_ACTIVATION_TITLE "产品未激活"
#define UI_TEXT_ACTIVATION_FORMAT "请先激活，激活码：%s"
#define UI_TEXT_ACTIVATION_FALLBACK "请先激活，激活码：未知"
#define UI_TEXT_UNKNOWN "未知"
#define UI_TEXT_ERROR_TITLE "连接失败"
#define UI_TEXT_OTA_TITLE "正在连接小智"
#define UI_TEXT_OTA_HINT "正在获取设备激活状态..."

extern const lv_font_t font_puhui_20_4;

typedef struct {
    const char *name;
    const char *emoji;
} xiaozhi_emoji_map_t;

static const xiaozhi_emoji_map_t s_emoji_map[] = {
    {"happy", "\xF0\x9F\x99\x82"},
    {"sad", "\xF0\x9F\x98\x94"},
    {"thinking", "\xF0\x9F\xA4\x94"},
    {"crying", "\xF0\x9F\x98\xAD"},
    {"angry", "\xF0\x9F\x98\xA0"},
    {"sleepy", "\xF0\x9F\x98\xB4"},
    {"neutral", "\xF0\x9F\x98\xB6"},
};

static bool s_ui_initialized;
static lv_obj_t *s_root;
static lv_obj_t *s_title_label;
static lv_obj_t *s_emoji_label;
static lv_obj_t *s_text_label;
static lv_obj_t *s_qrcode;

static const char *find_emoji(const char *name)
{
    const char *target = name != NULL && name[0] != '\0' ? name : "happy";

    for (size_t i = 0; i < sizeof(s_emoji_map) / sizeof(s_emoji_map[0]); ++i) {
        if (strcmp(s_emoji_map[i].name, target) == 0) {
            return s_emoji_map[i].emoji;
        }
    }

    return s_emoji_map[0].emoji;
}

static char *format_activation_message(const char *code)
{
    int required = snprintf(NULL, 0, UI_TEXT_ACTIVATION_FORMAT, code);
    if (required < 0) {
        return NULL;
    }

    char *message = (char *)malloc((size_t)required + 1);
    if (message == NULL) {
        return NULL;
    }

    int written = snprintf(message, (size_t)required + 1, UI_TEXT_ACTIVATION_FORMAT, code);
    if (written != required) {
        free(message);
        return NULL;
    }

    return message;
}

static void hide_qrcode(void)
{
    if (s_qrcode != NULL) {
        lv_obj_add_flag(s_qrcode, LV_OBJ_FLAG_HIDDEN);
    }
}

static void show_status_locked(const char *title, const char *emoji_name, const char *text)
{
    hide_qrcode();
    lv_obj_clear_flag(s_emoji_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_text_label, LV_OBJ_FLAG_HIDDEN);

    lv_label_set_text(s_title_label, XIAOZHI_UI_SAFE_TEXT(title));
    lv_label_set_text(s_emoji_label, find_emoji(emoji_name));
    lv_label_set_text(s_text_label, XIAOZHI_UI_SAFE_TEXT(text));
}

static void reset_objects_locked(void)
{
    if (s_root != NULL) {
        lv_obj_delete(s_root);
    }

    s_root = NULL;
    s_title_label = NULL;
    s_emoji_label = NULL;
    s_text_label = NULL;
    s_qrcode = NULL;
}

static esp_err_t create_objects_locked(void)
{
    s_root = lv_obj_create(NULL);
    if (s_root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    lv_obj_remove_style_all(s_root);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(0xF7F9FA), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_left(s_root, 10, 0);
    lv_obj_set_style_pad_right(s_root, 10, 0);
    lv_obj_set_style_pad_top(s_root, 12, 0);
    lv_obj_set_style_pad_bottom(s_root, 12, 0);

    s_title_label = lv_label_create(s_root);
    if (s_title_label == NULL) {
        goto no_mem;
    }
    lv_obj_set_width(s_title_label, BSP_LCD_H_RES - 20);
    lv_label_set_long_mode(s_title_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_title_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_title_label, lv_color_hex(0x15202B), 0);
    lv_obj_set_style_text_font(s_title_label, &font_puhui_20_4, 0);
    lv_obj_align(s_title_label, LV_ALIGN_TOP_MID, 0, 10);

    s_emoji_label = lv_label_create(s_root);
    if (s_emoji_label == NULL) {
        goto no_mem;
    }
    lv_obj_set_width(s_emoji_label, BSP_LCD_H_RES - 20);
    lv_obj_set_style_text_align(s_emoji_label, LV_TEXT_ALIGN_CENTER, 0);
    const lv_font_t *emoji_font = font_emoji_64_init();
    lv_obj_set_style_text_font(s_emoji_label, emoji_font != NULL ? emoji_font : &lv_font_montserrat_20, 0);
    lv_obj_align(s_emoji_label, LV_ALIGN_CENTER, 0, -28);

    s_text_label = lv_label_create(s_root);
    if (s_text_label == NULL) {
        goto no_mem;
    }
    lv_obj_set_width(s_text_label, BSP_LCD_H_RES - 22);
    lv_label_set_long_mode(s_text_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_text_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_text_label, lv_color_hex(0x334155), 0);
    lv_obj_set_style_text_font(s_text_label, &font_puhui_20_4, 0);
    lv_obj_set_style_text_line_space(s_text_label, 4, 0);
    lv_obj_align(s_text_label, LV_ALIGN_BOTTOM_MID, 0, -18);

    s_qrcode = lv_qrcode_create(s_root);
    if (s_qrcode == NULL) {
        goto no_mem;
    }
    lv_qrcode_set_size(s_qrcode, XIAOZHI_UI_QRCODE_SIZE);
    lv_qrcode_set_dark_color(s_qrcode, lv_color_hex(0x111111));
    lv_qrcode_set_light_color(s_qrcode, lv_color_hex(0xFFFFFF));
    lv_qrcode_set_quiet_zone(s_qrcode, true);
    lv_obj_set_style_border_color(s_qrcode, lv_color_hex(0xCBD5E1), 0);
    lv_obj_set_style_border_width(s_qrcode, 4, 0);
    lv_obj_set_style_radius(s_qrcode, 0, 0);
    lv_obj_clear_flag(s_qrcode, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_qrcode, LV_ALIGN_CENTER, 0, 6);
    lv_obj_add_flag(s_qrcode, LV_OBJ_FLAG_HIDDEN);

    lv_screen_load(s_root);
    show_status_locked(UI_TEXT_TITLE, "happy", UI_TEXT_HELLO);
    return ESP_OK;

no_mem:
    reset_objects_locked();
    return ESP_ERR_NO_MEM;
}

esp_err_t xiaozhi_ui_init(void)
{
    if (s_ui_initialized) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(display_service_init(), TAG, "init display service failed");

    lvgl_port_lock(0);
    esp_err_t err = create_objects_locked();
    lvgl_port_unlock();
    ESP_RETURN_ON_ERROR(err, TAG, "create xiaozhi ui objects failed");

    s_ui_initialized = true;
    ESP_LOGI(TAG, "xiaozhi ui initialized");
    return ESP_OK;
}

void xiaozhi_ui_show_qrcode(const char *payload)
{
    if (payload == NULL || payload[0] == '\0') {
        xiaozhi_ui_show_error(UI_TEXT_PROV_FAILED, UI_TEXT_QR_EMPTY);
        return;
    }

    if (xiaozhi_ui_init() != ESP_OK) {
        return;
    }

    lvgl_port_lock(0);
    lv_label_set_text(s_title_label, UI_TEXT_PROV_TITLE);
    lv_label_set_text(s_text_label, UI_TEXT_PROV_HINT);
    lv_obj_add_flag(s_emoji_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_text_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_qrcode, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(s_qrcode, LV_ALIGN_CENTER, 0, 4);

    if (lv_qrcode_update(s_qrcode, payload, strlen(payload)) != LV_RESULT_OK) {
        lv_obj_add_flag(s_qrcode, LV_OBJ_FLAG_HIDDEN);
        lvgl_port_unlock();
        ESP_LOGE(TAG, "update QR code failed");
        xiaozhi_ui_show_error(UI_TEXT_PROV_FAILED, UI_TEXT_QR_GEN_FAILED);
        return;
    }
    lvgl_port_unlock();
}

void xiaozhi_ui_show_title(const char *title)
{
    if (xiaozhi_ui_init() != ESP_OK) {
        return;
    }

    lvgl_port_lock(0);
    lv_label_set_text(s_title_label, XIAOZHI_UI_SAFE_TEXT(title));
    lvgl_port_unlock();
}

void xiaozhi_ui_show_text(const char *text)
{
    if (xiaozhi_ui_init() != ESP_OK) {
        return;
    }

    lvgl_port_lock(0);
    hide_qrcode();
    lv_obj_clear_flag(s_text_label, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_text_label, XIAOZHI_UI_SAFE_TEXT(text));
    lvgl_port_unlock();
}

void xiaozhi_ui_show_emoji(const char *emoji_name)
{
    if (xiaozhi_ui_init() != ESP_OK) {
        return;
    }

    lvgl_port_lock(0);
    hide_qrcode();
    lv_obj_clear_flag(s_emoji_label, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_emoji_label, find_emoji(emoji_name));
    lvgl_port_unlock();
}

void xiaozhi_ui_show_activation_required(const char *activation_code)
{
    if (xiaozhi_ui_init() != ESP_OK) {
        return;
    }

    const char *code = activation_code != NULL && activation_code[0] != '\0' ? activation_code : UI_TEXT_UNKNOWN;
    char *message = format_activation_message(code);
    if (message == NULL) {
        ESP_LOGW(TAG, "format activation message failed");
    }

    lvgl_port_lock(0);
    show_status_locked(UI_TEXT_ACTIVATION_TITLE, "crying", message != NULL ? message : UI_TEXT_ACTIVATION_FALLBACK);
    lvgl_port_unlock();

    free(message);
}

void xiaozhi_ui_show_welcome(void)
{
    if (xiaozhi_ui_init() != ESP_OK) {
        return;
    }

    lvgl_port_lock(0);
    show_status_locked(UI_TEXT_TITLE, "happy", UI_TEXT_HELLO);
    lvgl_port_unlock();
}

void xiaozhi_ui_show_error(const char *title, const char *message)
{
    if (xiaozhi_ui_init() != ESP_OK) {
        return;
    }

    lvgl_port_lock(0);
    show_status_locked(title != NULL && title[0] != '\0' ? title : UI_TEXT_ERROR_TITLE, "sad", XIAOZHI_UI_SAFE_TEXT(message));
    lvgl_port_unlock();
}

void xiaozhi_ui_show_ota_loading(void)
{
    if (xiaozhi_ui_init() != ESP_OK) {
        return;
    }

    lvgl_port_lock(0);
    show_status_locked(UI_TEXT_OTA_TITLE, "thinking", UI_TEXT_OTA_HINT);
    lvgl_port_unlock();
}
