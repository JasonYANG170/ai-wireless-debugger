#pragma once
#include "driver/gpio.h"

/* === UART1 (DUT serial bridge, via TXB0106) === */
#define PIN_UART1_TX         47   /* ESP32 -> TXB0106 A6 -> DUT */
#define PIN_UART1_RX         21   /* DUT -> TXB0106 A5 -> ESP32 */
#define UART1_PORT_NUM       UART_NUM_1

/* === SWD (GPIO bit-bang, via TXB0106) === */
#define PIN_SWD_SWCLK        13
#define PIN_SWD_SWDIO         14
#define PIN_SWD_NRST         12

/* === LCD (SPI2, ST7735S 80x160) === */
#define LCD_HOST             SPI2_HOST
#define PIN_LCD_MOSI         48
#define PIN_LCD_SCLK         45
#define PIN_LCD_DC           38
#define PIN_LCD_CS           40
#define PIN_LCD_RST          39
#define PIN_LCD_BL           41
#define LCD_H_RES            80
#define LCD_V_RES            160
#define LCD_PIXEL_CLK_HZ     (20 * 1000 * 1000)

/* === Buttons (active low) === */
#define PIN_BTN_SW1          1
#define PIN_BTN_SW2          2
#define PIN_BTN_SW3          42

/* === Signal monitors (Phase 2) === */
#define PIN_MON_PWM          4   /* PWM input: freq + duty (GPIO edge capture) */

/* === USB (native OTG, fixed pins) === */
/* IO19=USB_DM, IO20=USB_DP - configured by USB peripheral */
