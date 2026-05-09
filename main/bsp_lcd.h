#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BSP_LCD_HOST SPI2_HOST

#define BSP_LCD_PIN_NUM_RST GPIO_NUM_16
#define BSP_LCD_PIN_NUM_SCLK GPIO_NUM_47
#define BSP_LCD_PIN_NUM_DC GPIO_NUM_45
#define BSP_LCD_PIN_NUM_CS GPIO_NUM_21
#define BSP_LCD_PIN_NUM_MOSI GPIO_NUM_48
#define BSP_LCD_PIN_NUM_MISO (-1)
#define BSP_LCD_PIN_NUM_BK_LIGHT GPIO_NUM_40

#define BSP_LCD_H_RES 240
#define BSP_LCD_V_RES 320
#define BSP_LCD_PIXEL_CLOCK_HZ (20 * 1000 * 1000)
#define BSP_LCD_CMD_BITS 8
#define BSP_LCD_PARAM_BITS 8
#define BSP_LCD_SPI_MODE 0
#define BSP_LCD_TRANS_QUEUE_DEPTH 10
#define BSP_LCD_DRAW_LINES 20
#define BSP_LCD_BITS_PER_PIXEL 16
#define BSP_LCD_GAP_X 0
#define BSP_LCD_GAP_Y 0

#define EXAMPLE_LCD_BK_LIGHT_ON_LEVEL 1
#define EXAMPLE_LCD_BK_LIGHT_OFF_LEVEL !EXAMPLE_LCD_BK_LIGHT_ON_LEVEL

#define BSP_LCD_RGB_ORDER LCD_RGB_ELEMENT_ORDER_BGR
#define BSP_LCD_COLOR_INVERT false
#define BSP_LCD_SWAP_XY false
#define BSP_LCD_MIRROR_X true
#define BSP_LCD_MIRROR_Y true

#define BSP_LCD_COLOR_RED 0xF800
#define BSP_LCD_COLOR_GREEN 0x07E0
#define BSP_LCD_COLOR_BLUE 0x001F
#define BSP_LCD_COLOR_WHITE 0xFFFF
#define BSP_LCD_COLOR_BLACK 0x0000

typedef struct {
    uint16_t h_res;
    uint16_t v_res;
    bool swap_xy;
    bool mirror_x;
    bool mirror_y;
} bsp_lcd_display_config_t;

esp_err_t bsp_lcd_init(void);
esp_lcd_panel_handle_t bsp_lcd_get_panel(void);
esp_lcd_panel_io_handle_t bsp_lcd_get_io(void);
const bsp_lcd_display_config_t *bsp_lcd_get_display_config(void);
void bsp_lcd_backlight_on(void);
void bsp_lcd_backlight_off(void);
esp_err_t bsp_lcd_fill_color(uint16_t color);

#ifdef __cplusplus
}
#endif
