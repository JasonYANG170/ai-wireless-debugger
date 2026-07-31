#pragma once
#ifndef LCD_DRIVER_H
#define LCD_DRIVER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* === Display geometry (landscape mode) === */
#define LCD_WIDTH      160
#define LCD_HEIGHT     80
#define LCD_FG_W       8      /* font glyph width  */
#define LCD_FG_H       16     /* font glyph height */
#define LCD_COLS       (LCD_WIDTH  / LCD_FG_W)   /* 20 columns */
#define LCD_ROWS       (LCD_HEIGHT / LCD_FG_H)   /* 5 rows    */

/* === RGB565 color macros === */
#define RGB565(r,g,b)  ((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | (((b) & 0xF8) >> 3))

#define COLOR_BLACK    0x0000
#define COLOR_WHITE    0xFFFF
#define COLOR_RED      0xF800
#define COLOR_GREEN    0x07E0
#define COLOR_BLUE     0x001F
#define COLOR_YELLOW   0xFFE0
#define COLOR_CYAN     0x07FF
#define COLOR_MAGENTA  0xF81F
#define COLOR_GRAY     0x8410

/* === Public API === */

/**
 * Initialize the LCD: backlight GPIO, SPI2 bus, panel IO, ST7735S panel,
 * and allocate the framebuffer in PSRAM. Must be called once.
 */
esp_err_t lcd_init(void);

/**
 * Push the entire in-RAM framebuffer to the panel via esp_lcd_panel_draw_bitmap.
 * Call after any drawing operations to make them visible.
 */
esp_err_t lcd_flush(void);

/**
 * Fill the entire framebuffer with a single color.
 */
void lcd_fill(uint16_t color);

/**
 * Fill a rectangular region of the framebuffer.
 * Coordinates are clipped to the display bounds.
 */
void lcd_fill_rect(int x, int y, int w, int h, uint16_t color);

/**
 * Set a single pixel in the framebuffer. Out-of-bounds writes are ignored.
 */
void lcd_draw_pixel(int x, int y, uint16_t color);

/**
 * Draw a horizontal line of width 1 pixel. Clipped to bounds.
 */
void lcd_draw_hline(int x, int y, int w, uint16_t color);

/**
 * Draw a vertical line of width 1 pixel. Clipped to bounds.
 */
void lcd_draw_vline(int x, int y, int h, uint16_t color);

/**
 * Draw a rectangle outline (1px). Clipped to bounds.
 */
void lcd_draw_rect(int x, int y, int w, int h, uint16_t color);

/**
 * Draw a single 8x16 character at pixel (x, y).
 * fg is the foreground color, bg is the background color.
 */
void lcd_draw_char(int x, int y, char ch, uint16_t fg, uint16_t bg);

/**
 * Draw a null-terminated string of 8x16 characters starting at pixel (x, y).
 * Text is clipped at the right edge of the display.
 */
void lcd_draw_text(int x, int y, const char *text, uint16_t fg, uint16_t bg);

/**
 * Control the LCD backlight (GPIO41). true = on, false = off.
 */
void lcd_set_backlight(bool on);

/**
 * Set backlight brightness 0..100 percent (LEDC PWM).
 */
void lcd_set_brightness(int percent);

/**
 * Return a read-only pointer to the 160x80 RGB565 framebuffer, or NULL if the
 * LCD is not yet initialised. Used for remote screen mirroring (HTTP/WS).
 */
const uint16_t *lcd_get_fb(void);

#ifdef __cplusplus
}
#endif

#endif /* LCD_DRIVER_H */
