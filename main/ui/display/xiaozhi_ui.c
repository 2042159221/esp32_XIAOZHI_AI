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
#define UI_TEXT_TITLE "\xE5\xB0\x9A\xE7\xA1\x85\xE8\xB0\xB7" "AI" "\xE5\xB0\x8F\xE6\x99\xBA"
#define UI_TEXT_HELLO "\xE4\xBD\xA0\xE5\xA5\xBD,\xE6\x88\x91\xE6\x98\xAF\xE5\xB0\x8F\xE6\x99\xBA"
#define UI_TEXT_PROV_TITLE "\xE8\xAF\xB7\xE6\x89\xAB\xE7\xA0\x81\xE9\x85\x8D\xE7\xBD\x91!"
#define UI_TEXT_PROV_HINT "\xE8\xAF\xB7\xE4\xBD\xBF\xE7\x94\xA8\xE6\x89\x8B\xE6\x9C\xBA\xE6\x89\xAB\xE7\xA0\x81\xE5\xAE\x8C\xE6\x88\x90 WiFi \xE9\x85\x8D\xE7\xBD\x91"
#define UI_TEXT_PROV_FAILED "\xE9\x85\x8D\xE7\xBD\x91\xE5\xA4\xB1\xE8\xB4\xA5"
#define UI_TEXT_QR_EMPTY "\xE4\xBA\x8C\xE7\xBB\xB4\xE7\xA0\x81\xE5\x86\x85\xE5\xAE\xB9\xE4\xB8\xBA\xE7\xA9\xBA"
#define UI_TEXT_QR_GEN_FAILED "\xE4\xBA\x8C\xE7\xBB\xB4\xE7\xA0\x81\xE7\x94\x9F\xE6\x88\x90\xE5\xA4\xB1\xE8\xB4\xA5"
#define UI_TEXT_ACTIVATION_TITLE "\xE4\xBA\xA7\xE5\x93\x81\xE6\x9C\xAA\xE6\xBF\x80\xE6\xB4\xBB"
#define UI_TEXT_ACTIVATION_FORMAT "\xE8\xAF\xB7\xE5\x85\x88\xE6\xBF\x80\xE6\xB4\xBB\xEF\xBC\x8C\xE6\xBF\x80\xE6\xB4\xBB\xE7\xA0\x81\xEF\xBC\x9A%s"
#define UI_TEXT_UNKNOWN "\xE6\x9C\xAA\xE7\x9F\xA5"
#define UI_TEXT_ERROR_TITLE "\xE8\xBF\x9E\xE6\x8E\xA5\xE5\xA4\xB1\xE8\xB4\xA5"
#define UI_TEXT_OTA_TITLE "\xE6\xAD\xA3\xE5\x9C\xA8\xE8\xBF\x9E\xE6\x8E\xA5\xE5\xB0\x8F\xE6\x99\xBA"
#define UI_TEXT_OTA_HINT "\xE6\xAD\xA3\xE5\x9C\xA8\xE8\x8E\xB7\xE5\x8F\x96\xE8\xAE\xBE\xE5\xA4\x87\xE6\xBF\x80\xE6\xB4\xBB\xE7\x8A\xB6\xE6\x80\x81..."

extern const lv_font_t font_puhui_basic_16_4;
extern const lv_font_t font_puhui_basic_20_4;

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
    lv_obj_set_style_text_font(s_title_label, &font_puhui_basic_20_4, 0);
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
    lv_obj_set_style_text_font(s_text_label, &font_puhui_basic_16_4, 0);
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
    show_status_locked(UI_TEXT_TITLE, "happy", UI_TEXT_HELLO);
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
    char message[96];
    snprintf(message,
             sizeof(message),
             UI_TEXT_ACTIVATION_FORMAT,
             activation_code != NULL && activation_code[0] != '\0' ? activation_code : UI_TEXT_UNKNOWN);

    if (xiaozhi_ui_init() != ESP_OK) {
        return;
    }

    lvgl_port_lock(0);
    show_status_locked(UI_TEXT_ACTIVATION_TITLE, "crying", message);
    lvgl_port_unlock();
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
