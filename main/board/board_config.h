#pragma once

#include <stdbool.h>

#include "board_pins.h"
#include "esp_lcd_panel_vendor.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BOARD_LCD_H_RES 240
#define BOARD_LCD_V_RES 320
#define BOARD_LCD_PIXEL_CLOCK_HZ (20 * 1000 * 1000)
#define BOARD_LCD_CMD_BITS 8
#define BOARD_LCD_PARAM_BITS 8
#define BOARD_LCD_SPI_MODE 0
#define BOARD_LCD_TRANS_QUEUE_DEPTH 10
#define BOARD_LCD_DRAW_LINES 20
#define BOARD_LCD_BITS_PER_PIXEL 16
#define BOARD_LCD_GAP_X 0
#define BOARD_LCD_GAP_Y 0
#define BOARD_LCD_BACKLIGHT_ON_LEVEL 1
#define BOARD_LCD_BACKLIGHT_OFF_LEVEL (!BOARD_LCD_BACKLIGHT_ON_LEVEL)
#define BOARD_LCD_RGB_ORDER LCD_RGB_ELEMENT_ORDER_BGR
#define BOARD_LCD_COLOR_INVERT false
#define BOARD_LCD_SWAP_XY false
#define BOARD_LCD_MIRROR_X true
#define BOARD_LCD_MIRROR_Y true

#define BOARD_BUTTON_SW2 2
#define BOARD_BUTTON_SW3 3
#define BOARD_BUTTON_MAX_INDEX 3
#define BOARD_BUTTON_SW2_MIN_MV 0
#define BOARD_BUTTON_SW2_MAX_MV 400
#define BOARD_BUTTON_SW3_MIN_MV 1200
#define BOARD_BUTTON_SW3_MAX_MV 1800

#define BOARD_STATUS_LED_COUNT 2
#define BOARD_STATUS_LED_TASK_STACK 3072
#define BOARD_STATUS_LED_TASK_PRIORITY 4

#ifdef __cplusplus
}
#endif
