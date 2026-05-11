#include "display_service.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "bsp_lcd.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_lvgl_port_disp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "display_service";

#define DISPLAY_SERVICE_QRCODE_SIZE 176
#define DISPLAY_SERVICE_BUFFER_PIXELS (BSP_LCD_H_RES * 20)
#define DISPLAY_SERVICE_QRCODE_DARK_COLOR 0x000000
#define DISPLAY_SERVICE_QRCODE_LIGHT_COLOR 0xFFFFFF
#define DISPLAY_SERVICE_QRCODE_BORDER_COLOR 0xDDDDDD

static bool s_display_initialized;
static lv_display_t *s_lvgl_display;
static lv_obj_t *s_screen;
static lv_obj_t *s_title_label;
static lv_obj_t *s_hint_label;
static lv_obj_t *s_message_label;
static lv_obj_t *s_qrcode;

static void display_service_clear_center_area(void)
{
    if (s_qrcode != NULL) {
        lv_obj_del(s_qrcode);
        s_qrcode = NULL;
    }

    if (s_message_label != NULL) {
        lv_obj_del(s_message_label);
        s_message_label = NULL;
    }
}

static void display_service_create_base_screen(void)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(0xF7F7F7), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_screen, 16, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);

    s_title_label = lv_label_create(s_screen);
    lv_label_set_long_mode(s_title_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_title_label, lv_pct(100));
    lv_obj_set_style_text_align(s_title_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_title_label, lv_color_hex(0x111111), 0);
    lv_obj_set_style_text_font(s_title_label, &lv_font_montserrat_14, 0);
    lv_obj_align(s_title_label, LV_ALIGN_TOP_MID, 0, 8);

    s_hint_label = lv_label_create(s_screen);
    lv_label_set_long_mode(s_hint_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_hint_label, BSP_LCD_H_RES - 40);
    lv_obj_set_style_text_align(s_hint_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_hint_label, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_font(s_hint_label, &lv_font_montserrat_14, 0);
    lv_obj_align(s_hint_label, LV_ALIGN_BOTTOM_MID, 0, -12);

    lv_screen_load(s_screen);
}

esp_err_t display_service_init(void)
{
    if (s_display_initialized) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(bsp_lcd_init(), TAG, "init BSP LCD failed");

    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "init LVGL port failed");

    const bsp_lcd_display_config_t *display_config = bsp_lcd_get_display_config();
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = bsp_lcd_get_io(),
        .panel_handle = bsp_lcd_get_panel(),
        .buffer_size = DISPLAY_SERVICE_BUFFER_PIXELS,
        .double_buffer = false,
        .hres = display_config->h_res,
        .vres = display_config->v_res,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {
            .swap_xy = display_config->swap_xy,
            .mirror_x = display_config->mirror_x,
            .mirror_y = display_config->mirror_y,
        },
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
            .swap_bytes = false,
        },
    };
    s_lvgl_display = lvgl_port_add_disp(&disp_cfg);
    ESP_RETURN_ON_FALSE(s_lvgl_display != NULL, ESP_FAIL, TAG, "add LVGL display failed");

    lvgl_port_lock(0);
    display_service_create_base_screen();
    lvgl_port_unlock();

    s_display_initialized = true;
    ESP_LOGI(TAG, "display UI initialized");
    return ESP_OK;
}

bool display_service_is_initialized(void)
{
    return s_display_initialized;
}

void display_service_show_message(const char *title, const char *message)
{
    if (!s_display_initialized) {
        return;
    }

    lvgl_port_lock(0);
    display_service_clear_center_area();

    lv_label_set_text(s_title_label, title != NULL ? title : "Notice");
    lv_label_set_text(s_hint_label, "");

    s_message_label = lv_label_create(s_screen);
    lv_obj_set_width(s_message_label, BSP_LCD_H_RES - 48);
    lv_label_set_long_mode(s_message_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_message_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_message_label, lv_color_hex(0x222222), 0);
    lv_obj_set_style_text_font(s_message_label, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_message_label, message != NULL ? message : "");
    lv_obj_align(s_message_label, LV_ALIGN_CENTER, 0, 0);

    lvgl_port_unlock();
}

void display_service_show_qrcode(const char *payload)
{
    if (!s_display_initialized) {
        return;
    }

    if (payload == NULL || payload[0] == '\0') {
        ESP_LOGW(TAG, "provisioning payload is empty");
        display_service_show_message("Provisioning", "QR payload is empty");
        return;
    }

    lvgl_port_lock(0);
    display_service_clear_center_area();

    lv_label_set_text(s_title_label, "XiaoZhi Wi-Fi Setup");
    lv_label_set_text(s_hint_label, "Open Espressif Provisioning app and scan this code");

    s_qrcode = lv_qrcode_create(s_screen);
    if (s_qrcode == NULL) {
        lvgl_port_unlock();
        ESP_LOGE(TAG, "create QR code widget failed");
        display_service_show_message("Provisioning", "Failed to create QR code");
        return;
    }

    lv_qrcode_set_size(s_qrcode, DISPLAY_SERVICE_QRCODE_SIZE);
    lv_qrcode_set_dark_color(s_qrcode, lv_color_hex(DISPLAY_SERVICE_QRCODE_DARK_COLOR));
    lv_qrcode_set_light_color(s_qrcode, lv_color_hex(DISPLAY_SERVICE_QRCODE_LIGHT_COLOR));
    lv_qrcode_set_quiet_zone(s_qrcode, true);
    lv_obj_set_style_border_color(s_qrcode, lv_color_hex(DISPLAY_SERVICE_QRCODE_BORDER_COLOR), 0);
    lv_obj_set_style_border_width(s_qrcode, 5, 0);
    lv_obj_set_style_radius(s_qrcode, 0, 0);
    lv_obj_clear_flag(s_qrcode, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_qrcode, LV_ALIGN_CENTER, 0, 4);

    if (lv_qrcode_update(s_qrcode, payload, strlen(payload)) != LV_RESULT_OK) {
        lv_obj_del(s_qrcode);
        s_qrcode = NULL;
        lvgl_port_unlock();
        ESP_LOGE(TAG, "update QR code widget failed");
        display_service_show_message("Provisioning", "QR update failed. Check serial log");
        return;
    }

    lvgl_port_unlock();
}

void display_service_show_color_bars_test(void)
{
    if (!s_display_initialized) {
        return;
    }

    static const uint16_t test_colors[] = {
        BSP_LCD_COLOR_RED,
        BSP_LCD_COLOR_GREEN,
        BSP_LCD_COLOR_BLUE,
        BSP_LCD_COLOR_WHITE,
        BSP_LCD_COLOR_BLACK,
    };

    lvgl_port_lock(0);
    for (size_t index = 0; index < sizeof(test_colors) / sizeof(test_colors[0]); ++index) {
        esp_err_t err = bsp_lcd_fill_color(test_colors[index]);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "fill test color failed: %s", esp_err_to_name(err));
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    lvgl_port_unlock();
}
