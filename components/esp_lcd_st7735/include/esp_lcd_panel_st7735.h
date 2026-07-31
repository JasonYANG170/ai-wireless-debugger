#pragma once
#include "esp_lcd_panel_dev.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create LCD panel for model ST7735S (80x160 RGB)
 * @param io Panel IO handle (from esp_lcd_new_panel_io_spi)
 * @param panel_dev_config Device config (bits_per_pixel=16, rgb_ele_order=BGR)
 * @param ret_panel Output panel handle
 * @return ESP_OK on success
 */
esp_err_t esp_lcd_new_panel_st7735(const esp_lcd_panel_io_handle_t io,
                                     const esp_lcd_panel_dev_config_t *panel_dev_config,
                                     esp_lcd_panel_handle_t *ret_panel);

#ifdef __cplusplus
}
#endif
