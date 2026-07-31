/*
 * lcd_driver.c - ST7735S 80x160 RGB565 LCD driver for ESP32-S3
 *
 * Provides a PSRAM-backed framebuffer with simple drawing primitives:
 * fill, fill_rect, pixel, hline, vline, rect, 8x16 text, and flush.
 */
#include "lcd_driver.h"
#include "lcd_font.h"
#include "cjk_font.h"
#include "pinout.h"

#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7735.h"

static const char *TAG = "lcd_driver";

/* --- File-scope state --- */
static esp_lcd_panel_io_handle_t s_io   = NULL;
static esp_lcd_panel_handle_t     s_panel = NULL;
static uint16_t                  *s_fb   = NULL;   /* framebuffer in PSRAM */

#define FB_SIZE  (LCD_WIDTH * LCD_HEIGHT)            /* 12800 pixels  */
#define FB_BYTES (FB_SIZE * sizeof(uint16_t))        /* 25600 bytes   */

/* ------------------------------------------------------------------ *
 *  Local helpers
 * ------------------------------------------------------------------ */

/* Bounds-checked pixel index; returns -1 if out of range. */
static inline int fb_index(int x, int y)
{
    if (x < 0 || x >= LCD_WIDTH || y < 0 || y >= LCD_HEIGHT) {
        return -1;
    }
    return y * LCD_WIDTH + x;
}

/* ------------------------------------------------------------------ *
 *  Public API
 * ------------------------------------------------------------------ */

esp_err_t lcd_init(void)
{
    esp_err_t ret = ESP_OK;

    /* ---- 1. Backlight GPIO (IO41), default ON ---- */
    gpio_config_t bl_conf = {
        .pin_bit_mask = 1ULL << PIN_LCD_BL,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&bl_conf), TAG, "BL gpio_config failed");
    gpio_set_level(PIN_LCD_BL, 1);   /* backlight on */

    /* ---- 2. SPI2 bus ---- */
    spi_bus_config_t buscfg = {
        .mosi_io_num     = PIN_LCD_MOSI,
        .miso_io_num     = -1,                       /* no MISO for write-only LCD */
        .sclk_io_num     = PIN_LCD_SCLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = FB_BYTES + 8,             /* full frame + cmd overhead */
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO),
                        TAG, "SPI bus init failed");

    /* ---- 3. Panel IO (DC selects cmd vs data over SPI) ---- */
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num         = PIN_LCD_DC,
        .cs_gpio_num         = PIN_LCD_CS,
        .pclk_hz             = LCD_PIXEL_CLK_HZ,
        .lcd_cmd_bits        = 8,
        .lcd_param_bits      = 8,
        .spi_mode            = 0,
        .trans_queue_depth   = 10,
        .on_color_trans_done = NULL,
        .user_ctx            = NULL,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(LCD_HOST, &io_config, &s_io),
                        TAG, "panel io init failed");

    /* ---- 4. ST7735S panel ---- */
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_LCD_RST,
        .bits_per_pixel = 16,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .flags = {
            .reset_active_high = 0,   /* RST active low */
        },
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7735(s_io, &panel_config, &s_panel),
                        TAG, "new panel st7735 failed");

    /* ---- 5. Reset -> init -> gap ---- */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel),       TAG, "panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel),         TAG, "panel init failed");
    /* 80x160 RGB module: column offset 24, row offset 0 */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_panel, 24, 0), TAG, "set gap failed");

    /* ---- 6. Framebuffer in PSRAM ---- */
    s_fb = heap_caps_malloc(FB_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_fb) {
        ESP_LOGE(TAG, "PSRAM fb alloc failed (%d bytes)", FB_BYTES);
        return ESP_ERR_NO_MEM;
    }
    memset(s_fb, 0, FB_BYTES);

    ESP_LOGI(TAG, "LCD init OK: %dx%d RGB565, fb=%p (%d bytes PSRAM)",
             LCD_WIDTH, LCD_HEIGHT, (void *)s_fb, FB_BYTES);
    return ESP_OK;
}

/* Physical panel dimensions (portrait) */
#define PANEL_W 80
#define PANEL_H 160

/* Landscape framebuffer dimensions */
#define FB_W LCD_WIDTH    /* 160 */
#define FB_H LCD_HEIGHT   /* 80  */

esp_err_t lcd_flush(void)
{
    if (!s_panel || !s_fb) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Software rotation 90 CW (270 degrees): landscape fb (160x80) -> portrait panel (80x160)
     * Mapping: fb_row = panel_col, fb_col = PANEL_H-1-panel_row */
    static uint16_t *rotbuf = NULL;
    if (!rotbuf) {
        rotbuf = heap_caps_malloc(PANEL_W * PANEL_H * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        if (!rotbuf) return ESP_ERR_NO_MEM;
    }
    for (int prow = 0; prow < PANEL_H; prow++) {
        for (int pcol = 0; pcol < PANEL_W; pcol++) {
            int fr = pcol;
            int fc = PANEL_H - 1 - prow;
            rotbuf[prow * PANEL_W + pcol] = s_fb[fr * FB_W + fc];
        }
    }
    return esp_lcd_panel_draw_bitmap(s_panel, 0, 0, PANEL_W, PANEL_H, rotbuf);
}

const uint16_t *lcd_get_fb(void)
{
    return s_fb;
}

void lcd_fill(uint16_t color)
{
    if (!s_fb) return;
    /* 16-bit fill: use memset pattern only works for 0x0000/0xFFFF; else word-fill */
    for (int i = 0; i < FB_SIZE; i++) {
        s_fb[i] = color;
    }
}

void lcd_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (!s_fb) return;
    /* Clip to display bounds */
    int x0 = x, y0 = y;
    int x1 = x + w, y1 = y + h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > LCD_WIDTH)  x1 = LCD_WIDTH;
    if (y1 > LCD_HEIGHT) y1 = LCD_HEIGHT;
    if (x0 >= x1 || y0 >= y1) return;

    for (int yy = y0; yy < y1; yy++) {
        uint16_t *row = &s_fb[yy * LCD_WIDTH];
        for (int xx = x0; xx < x1; xx++) {
            row[xx] = color;
        }
    }
}

void lcd_draw_pixel(int x, int y, uint16_t color)
{
    int idx = fb_index(x, y);
    if (idx < 0) return;
    s_fb[idx] = color;
}

void lcd_draw_hline(int x, int y, int w, uint16_t color)
{
    if (!s_fb) return;
    int x1 = x + w;
    if (y < 0 || y >= LCD_HEIGHT) return;
    if (x < 0)  { w  += x; x = 0; }
    if (x1 > LCD_WIDTH) x1 = LCD_WIDTH;
    if (x >= x1) return;
    uint16_t *row = &s_fb[y * LCD_WIDTH];
    for (int xx = x; xx < x1; xx++) {
        row[xx] = color;
    }
}

void lcd_draw_vline(int x, int y, int h, uint16_t color)
{
    if (!s_fb) return;
    int y1 = y + h;
    if (x < 0 || x >= LCD_WIDTH) return;
    if (y < 0)  { h  += y; y = 0; }
    if (y1 > LCD_HEIGHT) y1 = LCD_HEIGHT;
    if (y >= y1) return;
    for (int yy = y; yy < y1; yy++) {
        s_fb[yy * LCD_WIDTH + x] = color;
    }
}

void lcd_draw_rect(int x, int y, int w, int h, uint16_t color)
{
    if (w <= 0 || h <= 0) return;
    lcd_draw_hline(x, y, w, color);            /* top    */
    lcd_draw_hline(x, y + h - 1, w, color);    /* bottom */
    lcd_draw_vline(x, y, h, color);            /* left   */
    lcd_draw_vline(x + w - 1, y, h, color);    /* right  */
}

void lcd_draw_char(int x, int y, char ch, uint16_t fg, uint16_t bg)
{
    if (!s_fb) return;
    const uint8_t *glyph = font_8x16[font_8x16_index(ch)];

    for (int gy = 0; gy < LCD_FG_H; gy++) {
        uint8_t bits = glyph[gy];
        uint16_t *row = &s_fb[(y + gy) * LCD_WIDTH];
        for (int gx = 0; gx < LCD_FG_W; gx++) {
            int px = x + gx;
            if (px < 0 || px >= LCD_WIDTH) continue;
            if (y + gy < 0 || y + gy >= LCD_HEIGHT) break;
            /* bit 7 = leftmost pixel */
            row[px] = (bits & (0x80 >> gx)) ? fg : bg;
        }
    }
}

/* Draw a 16x16 CJK character from HZK16 bitmap data */
static void lcd_draw_cjk_char(int x, int y, const uint8_t *bitmap, uint16_t fg, uint16_t bg)
{
    for (int gy = 0; gy < 16; gy++) {
        uint16_t bits = (bitmap[gy * 2] << 8) | bitmap[gy * 2 + 1];
        for (int gx = 0; gx < 16; gx++) {
            int px = x + gx, py = y + gy;
            if (px < 0 || px >= LCD_WIDTH) continue;
            if (py < 0 || py >= LCD_HEIGHT) break;
            s_fb[py * LCD_WIDTH + px] = (bits & (0x8000 >> gx)) ? fg : bg;
        }
    }
}

/* Draw a 16x16 placeholder box for unmapped non-ASCII characters */
static void lcd_draw_cjk_placeholder(int x, int y, uint16_t fg, uint16_t bg)
{
    if (x + 16 > LCD_WIDTH || y + 16 > LCD_HEIGHT) return;
    lcd_fill_rect(x, y, 16, 16, bg);
    lcd_draw_rect(x, y, 16, 16, fg);
    lcd_draw_hline(x + 3, y + 3, 10, fg);
    lcd_draw_hline(x + 3, y + 12, 10, fg);
}

void lcd_draw_text(int x, int y, const char *text, uint16_t fg, uint16_t bg)
{
    if (!s_fb || !text) return;
    int cx = x;
    for (const char *p = text; *p != '\0'; ) {
        unsigned char c = (unsigned char)*p;

        if (c < 0x80) {
            /* ASCII: 1 byte, 8px wide */
            if (cx + LCD_FG_W > LCD_WIDTH) break;
            lcd_draw_char(cx, y, (char)c, fg, bg);
            cx += LCD_FG_W;
            p++;
        } else if ((c & 0xF0) == 0xE0) {
            /* 3-byte UTF-8 (CJK) - try HZK16 lookup */
            if (cx + 16 > LCD_WIDTH) break;
            const uint8_t *bmp = cjk_get_bitmap_utf8((const uint8_t *)p, 3);
            if (bmp) {
                lcd_draw_cjk_char(cx, y, bmp, fg, bg);
            } else {
                lcd_draw_cjk_placeholder(cx, y, fg, bg);
            }
            cx += 16;
            p += 3;
        } else if ((c & 0xE0) == 0xC0) {
            /* 2-byte UTF-8 */
            if (cx + 16 > LCD_WIDTH) break;
            lcd_draw_cjk_placeholder(cx, y, fg, bg);
            cx += 16;
            p += 2;
        } else if ((c & 0xF8) == 0xF0) {
            /* 4-byte UTF-8 */
            if (cx + 16 > LCD_WIDTH) break;
            lcd_draw_cjk_placeholder(cx, y, fg, bg);
            cx += 16;
            p += 4;
        } else {
            p++;
        }
    }
}

void lcd_set_backlight(bool on)
{
    lcd_set_brightness(on ? 100 : 0);
}

/* ---- Brightness via LEDC PWM on the backlight pin ---- */
#include "driver/ledc.h"
static bool s_bright_inited = false;

void lcd_set_brightness(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    if (!s_bright_inited) {
        ledc_timer_config_t t = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .duty_resolution = LEDC_TIMER_10_BIT,
            .timer_num = LEDC_TIMER_1,
            .freq_hz = 1000,
            .clk_cfg = LEDC_AUTO_CLK,
        };
        ledc_timer_config(&t);
        ledc_channel_config_t c = {
            .gpio_num = PIN_LCD_BL,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = LEDC_CHANNEL_1,
            .timer_sel = LEDC_TIMER_1,
            .duty = (uint32_t)percent * 1023 / 100,
            .hpoint = 0,
        };
        ledc_channel_config(&c);
        s_bright_inited = true;
        return;
    }
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, (uint32_t)percent * 1023 / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}
