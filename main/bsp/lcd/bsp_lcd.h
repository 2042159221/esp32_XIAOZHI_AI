#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "board_config.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BSP_LCD_HOST BOARD_LCD_SPI_HOST
#define BSP_LCD_PIN_NUM_RST BOARD_LCD_PIN_NUM_RST
#define BSP_LCD_PIN_NUM_SCLK BOARD_LCD_PIN_NUM_SCLK
#define BSP_LCD_PIN_NUM_DC BOARD_LCD_PIN_NUM_DC
#define BSP_LCD_PIN_NUM_CS BOARD_LCD_PIN_NUM_CS
#define BSP_LCD_PIN_NUM_MOSI BOARD_LCD_PIN_NUM_MOSI
#define BSP_LCD_PIN_NUM_MISO BOARD_LCD_PIN_NUM_MISO
#define BSP_LCD_PIN_NUM_BK_LIGHT BOARD_LCD_PIN_NUM_BK_LIGHT

#define BSP_LCD_H_RES BOARD_LCD_H_RES
#define BSP_LCD_V_RES BOARD_LCD_V_RES
#define BSP_LCD_PIXEL_CLOCK_HZ BOARD_LCD_PIXEL_CLOCK_HZ
#define BSP_LCD_CMD_BITS BOARD_LCD_CMD_BITS
#define BSP_LCD_PARAM_BITS BOARD_LCD_PARAM_BITS
#define BSP_LCD_SPI_MODE BOARD_LCD_SPI_MODE
#define BSP_LCD_TRANS_QUEUE_DEPTH BOARD_LCD_TRANS_QUEUE_DEPTH
#define BSP_LCD_DRAW_LINES BOARD_LCD_DRAW_LINES
#define BSP_LCD_BITS_PER_PIXEL BOARD_LCD_BITS_PER_PIXEL
#define BSP_LCD_GAP_X BOARD_LCD_GAP_X
#define BSP_LCD_GAP_Y BOARD_LCD_GAP_Y
#define BSP_LCD_BK_LIGHT_ON_LEVEL BOARD_LCD_BACKLIGHT_ON_LEVEL
#define BSP_LCD_BK_LIGHT_OFF_LEVEL BOARD_LCD_BACKLIGHT_OFF_LEVEL
#define BSP_LCD_RGB_ORDER BOARD_LCD_RGB_ORDER
#define BSP_LCD_COLOR_INVERT BOARD_LCD_COLOR_INVERT
#define BSP_LCD_SWAP_XY BOARD_LCD_SWAP_XY
#define BSP_LCD_MIRROR_X BOARD_LCD_MIRROR_X
#define BSP_LCD_MIRROR_Y BOARD_LCD_MIRROR_Y

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
