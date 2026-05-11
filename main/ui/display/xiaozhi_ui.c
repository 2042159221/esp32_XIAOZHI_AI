#include "xiaozhi_ui.h"

#include <stdio.h>
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

extern const lv_font_t font_puhui_16_4;
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

static void create_objects_locked(void)
{
    s_root = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(0xF7F9FA), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_left(s_root, 14, 0);
    lv_obj_set_style_pad_right(s_root, 14, 0);
    lv_obj_set_style_pad_top(s_root, 12, 0);
    lv_obj_set_style_pad_bottom(s_root, 12, 0);

    s_title_label = lv_label_create(s_root);
    lv_obj_set_width(s_title_label, BSP_LCD_H_RES - 28);
    lv_label_set_long_mode(s_title_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_title_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_title_label, lv_color_hex(0x15202B), 0);
    lv_obj_set_style_text_font(s_title_label, &font_puhui_20_4, 0);
    lv_obj_align(s_title_label, LV_ALIGN_TOP_MID, 0, 10);

    s_emoji_label = lv_label_create(s_root);
    lv_obj_set_width(s_emoji_label, BSP_LCD_H_RES - 28);
    lv_obj_set_style_text_align(s_emoji_label, LV_TEXT_ALIGN_CENTER, 0);
    const lv_font_t *emoji_font = font_emoji_64_init();
    lv_obj_set_style_text_font(s_emoji_label, emoji_font != NULL ? emoji_font : &lv_font_montserrat_20, 0);
    lv_obj_align(s_emoji_label, LV_ALIGN_CENTER, 0, -28);

    s_text_label = lv_label_create(s_root);
    lv_obj_set_width(s_text_label, BSP_LCD_H_RES - 34);
    lv_label_set_long_mode(s_text_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_text_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_text_label, lv_color_hex(0x334155), 0);
    lv_obj_set_style_text_font(s_text_label, &font_puhui_16_4, 0);
    lv_obj_align(s_text_label, LV_ALIGN_BOTTOM_MID, 0, -26);

    s_qrcode = lv_qrcode_create(s_root);
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
    show_status_locked("尚硅谷AI小智", "happy", "你好,我是小智");
}

esp_err_t xiaozhi_ui_init(void)
{
    if (s_ui_initialized) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(display_service_init(), TAG, "init display service failed");

    lvgl_port_lock(0);
    create_objects_locked();
    lvgl_port_unlock();

    s_ui_initialized = true;
    ESP_LOGI(TAG, "xiaozhi ui initialized");
    return ESP_OK;
}

void xiaozhi_ui_show_qrcode(const char *payload)
{
    if (payload == NULL || payload[0] == '\0') {
        xiaozhi_ui_show_error("配网失败", "二维码内容为空");
        return;
    }

    if (xiaozhi_ui_init() != ESP_OK) {
        return;
    }

    lvgl_port_lock(0);
    lv_label_set_text(s_title_label, "请扫码配网!");
    lv_label_set_text(s_text_label, "请使用手机扫码完成 WiFi 配网");
    lv_obj_add_flag(s_emoji_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_text_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_qrcode, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(s_qrcode, LV_ALIGN_CENTER, 0, 4);

    if (lv_qrcode_update(s_qrcode, payload, strlen(payload)) != LV_RESULT_OK) {
        lv_obj_add_flag(s_qrcode, LV_OBJ_FLAG_HIDDEN);
        lvgl_port_unlock();
        ESP_LOGE(TAG, "update QR code failed");
        xiaozhi_ui_show_error("配网失败", "二维码生成失败");
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
    char message[96];
    snprintf(message,
             sizeof(message),
             "请先激活，激活码：%s",
             activation_code != NULL && activation_code[0] != '\0' ? activation_code : "未知");

    if (xiaozhi_ui_init() != ESP_OK) {
        return;
    }

    lvgl_port_lock(0);
    show_status_locked("产品未激活", "crying", message);
    lvgl_port_unlock();
}

void xiaozhi_ui_show_welcome(void)
{
    if (xiaozhi_ui_init() != ESP_OK) {
        return;
    }

    lvgl_port_lock(0);
    show_status_locked("尚硅谷AI小智", "happy", "你好,我是小智");
    lvgl_port_unlock();
}

void xiaozhi_ui_show_error(const char *title, const char *message)
{
    if (xiaozhi_ui_init() != ESP_OK) {
        return;
    }

    lvgl_port_lock(0);
    show_status_locked(title != NULL && title[0] != '\0' ? title : "连接失败", "sad", XIAOZHI_UI_SAFE_TEXT(message));
    lvgl_port_unlock();
}

void xiaozhi_ui_show_ota_loading(void)
{
    if (xiaozhi_ui_init() != ESP_OK) {
        return;
    }

    lvgl_port_lock(0);
    show_status_locked("正在连接小智", "thinking", "正在获取设备激活状态...");
    lvgl_port_unlock();
}
