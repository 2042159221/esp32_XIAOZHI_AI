#pragma once

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "hal/adc_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BOARD_LCD_SPI_HOST SPI2_HOST
#define BOARD_LCD_PIN_NUM_RST GPIO_NUM_16
#define BOARD_LCD_PIN_NUM_SCLK GPIO_NUM_47
#define BOARD_LCD_PIN_NUM_DC GPIO_NUM_45
#define BOARD_LCD_PIN_NUM_CS GPIO_NUM_21
#define BOARD_LCD_PIN_NUM_MOSI GPIO_NUM_48
#define BOARD_LCD_PIN_NUM_MISO (-1)
#define BOARD_LCD_PIN_NUM_BK_LIGHT GPIO_NUM_40

#define BOARD_BUTTON_ADC_UNIT ADC_UNIT_1
#define BOARD_BUTTON_ADC_CHANNEL 7

#define BOARD_STATUS_LED_GPIO GPIO_NUM_46

#ifdef __cplusplus
}
#endif
