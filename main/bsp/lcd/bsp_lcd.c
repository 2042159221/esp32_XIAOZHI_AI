#include "bsp_lcd.h"

#include <stdlib.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"

static const char *TAG = "bsp_lcd";

static esp_lcd_panel_io_handle_t s_panel_io;
static esp_lcd_panel_handle_t s_panel;
static bool s_backlight_initialized;
static bool s_lcd_initialized;
static const bsp_lcd_display_config_t s_display_config = {
    .h_res = BSP_LCD_H_RES,
    .v_res = BSP_LCD_V_RES,
    .swap_xy = BSP_LCD_SWAP_XY,
    .mirror_x = BSP_LCD_MIRROR_X,
    .mirror_y = BSP_LCD_MIRROR_Y,
};

static esp_err_t bsp_lcd_init_backlight(void)
{
    if (s_backlight_initialized) {
        return ESP_OK;
    }

    const gpio_config_t bk_gpio_config = {
        .pin_bit_mask = 1ULL << BSP_LCD_PIN_NUM_BK_LIGHT,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&bk_gpio_config), TAG, "configure LCD backlight gpio failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(BSP_LCD_PIN_NUM_BK_LIGHT, BSP_LCD_BK_LIGHT_OFF_LEVEL), TAG, "turn off LCD backlight failed");

    s_backlight_initialized = true;
    return ESP_OK;
}

esp_err_t bsp_lcd_init(void)
{
    if (s_lcd_initialized) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(bsp_lcd_init_backlight(), TAG, "init LCD backlight failed");

    const spi_bus_config_t bus_config = {
        .sclk_io_num = BSP_LCD_PIN_NUM_SCLK,
        .mosi_io_num = BSP_LCD_PIN_NUM_MOSI,
        .miso_io_num = BSP_LCD_PIN_NUM_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BSP_LCD_H_RES * BSP_LCD_DRAW_LINES * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BSP_LCD_HOST, &bus_config, SPI_DMA_CH_AUTO), TAG, "initialize LCD SPI bus failed");

    const esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = BSP_LCD_PIN_NUM_CS,
        .dc_gpio_num = BSP_LCD_PIN_NUM_DC,
        .spi_mode = BSP_LCD_SPI_MODE,
        .pclk_hz = BSP_LCD_PIXEL_CLOCK_HZ,
        .trans_queue_depth = BSP_LCD_TRANS_QUEUE_DEPTH,
        .lcd_cmd_bits = BSP_LCD_CMD_BITS,
        .lcd_param_bits = BSP_LCD_PARAM_BITS,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_HOST, &io_config, &s_panel_io), TAG, "create LCD panel IO failed");

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BSP_LCD_PIN_NUM_RST,
        .rgb_ele_order = BSP_LCD_RGB_ORDER,
        .bits_per_pixel = BSP_LCD_BITS_PER_PIXEL,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(s_panel_io, &panel_config, &s_panel), TAG, "create ST7789 panel failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset ST7789 panel failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init ST7789 panel failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, BSP_LCD_COLOR_INVERT), TAG, "set LCD color invert failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_panel, BSP_LCD_SWAP_XY), TAG, "set LCD swap_xy failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, BSP_LCD_MIRROR_X, BSP_LCD_MIRROR_Y), TAG, "set LCD mirror failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_panel, BSP_LCD_GAP_X, BSP_LCD_GAP_Y), TAG, "set LCD gap failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "turn on LCD display failed");

    s_lcd_initialized = true;
    bsp_lcd_backlight_on();
    ESP_LOGI(TAG, "LCD initialized: %ux%u, swap_xy=%d, mirror_x=%d, mirror_y=%d, pclk=%dHz",
             s_display_config.h_res,
             s_display_config.v_res,
             s_display_config.swap_xy,
             s_display_config.mirror_x,
             s_display_config.mirror_y,
             BSP_LCD_PIXEL_CLOCK_HZ);
    return ESP_OK;
}

esp_lcd_panel_handle_t bsp_lcd_get_panel(void)
{
    return s_panel;
}

esp_lcd_panel_io_handle_t bsp_lcd_get_io(void)
{
    return s_panel_io;
}

const bsp_lcd_display_config_t *bsp_lcd_get_display_config(void)
{
    return &s_display_config;
}

void bsp_lcd_backlight_on(void)
{
    if (s_backlight_initialized) {
        gpio_set_level(BSP_LCD_PIN_NUM_BK_LIGHT, BSP_LCD_BK_LIGHT_ON_LEVEL);
    }
}

void bsp_lcd_backlight_off(void)
{
    if (s_backlight_initialized) {
        gpio_set_level(BSP_LCD_PIN_NUM_BK_LIGHT, BSP_LCD_BK_LIGHT_OFF_LEVEL);
    }
}

esp_err_t bsp_lcd_fill_color(uint16_t color)
{
    ESP_RETURN_ON_FALSE(s_lcd_initialized, ESP_ERR_INVALID_STATE, TAG, "LCD is not initialized");

    const size_t line_pixels = BSP_LCD_H_RES * BSP_LCD_DRAW_LINES;
    uint16_t *line_buffer = heap_caps_malloc(line_pixels * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_RETURN_ON_FALSE(line_buffer != NULL, ESP_ERR_NO_MEM, TAG, "allocate LCD fill buffer failed");

    for (size_t index = 0; index < line_pixels; ++index) {
        line_buffer[index] = color;
    }

    esp_err_t err = ESP_OK;
    for (int y = 0; y < BSP_LCD_V_RES; y += BSP_LCD_DRAW_LINES) {
        int y_end = y + BSP_LCD_DRAW_LINES;
        if (y_end > BSP_LCD_V_RES) {
            y_end = BSP_LCD_V_RES;
        }

        err = esp_lcd_panel_draw_bitmap(s_panel, 0, y, BSP_LCD_H_RES, y_end, line_buffer);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "fill LCD color failed at line %d: %s", y, esp_err_to_name(err));
            break;
        }
    }

    free(line_buffer);
    return err;
}
