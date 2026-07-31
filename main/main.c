/*
 * Wireless Serial Debugger - Main Entry Point
 * ESP32-S3-WROOM-1U-N16R8
 * Landscape LCD menu UI + TCP serial bridge (3333) + HTTP status (80)
 */
#include <string.h>
#include "esp_log.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pinout.h"
#include "lcd_driver.h"
#include "menu_ui.h"
#include "wifi_manager.h"
#include "serial_bridge.h"
#include "swd_bridge.h"
#include "pin_config.h"
#include "pwm_mon.h"
#include "spi_mon.h"
#include "i2c_mon.h"
#include "buttons.h"
#include "tcp_server.h"
#include "dap_server.h"
#include "http_status.h"

static const char *TAG = "main";

static void on_serial_rx(const uint8_t *data, size_t len)
{
    tcp_server_broadcast(data, len);
    menu_push_rx_data(data, len);
}

static void on_button_event(button_id_t btn, button_event_t event)
{
    if (event == BTN_EVENT_PRESS) {
        switch (btn) {
        case BTN_SW1:
            /* SW1 = next item */
            menu_on_sw3_press();
            break;
        case BTN_SW2:
            menu_on_sw2_press();
            break;
        case BTN_SW3:
            /* SW3 = prev item */
            menu_on_sw1_press();
            break;
        default: break;
        }
    } else if (event == BTN_EVENT_LONG_PRESS) {
        switch (btn) {
        case BTN_SW1:
            /* Long press SW1: AP config mode */
            wifi_manager_start_ap();
            break;
        case BTN_SW2:
            /* Long press SW2: exit subpage OR read IDCODE on SWD page */
            menu_on_sw2_long_press();
            break;
        case BTN_SW3: {
            /* Long press SW3: cycle baud rate */
            static const uint32_t bauds[] = {9600, 115200, 460800, 921600};
            static int idx = 1;
            idx = (idx + 1) % 4;
            serial_bridge_set_baud(bauds[idx]);
            break;
        }
        default: break;
        }
    } else if (event == BTN_EVENT_HOLD) {
        switch (btn) {
        case BTN_SW2:
            /* Hold SW2: page-specific hold action (I2C mode toggle) */
            menu_on_sw2_hold();
            break;
        default: break;
        }
    }
}

/* UI render task (5Hz) */
static void ui_task(void *arg)
{
    while (1) {
        menu_render();
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Wireless Serial Debugger booting...");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(lcd_init());
    menu_init();

    wifi_manager_init();
    pin_config_init();
    serial_bridge_init(SERIAL_BAUD_DEFAULT);
    serial_bridge_set_rx_callback(on_serial_rx);
    swd_init();
    if (pin_config_pwm() >= 0) {
        pwm_mon_start(pin_config_pwm());
    }
    {
        int sck = pin_config_spi_sck(), mosi = pin_config_spi_mosi();
        int miso = pin_config_spi_miso(), cs = pin_config_spi_cs();
        if (sck >= 0 && mosi >= 0 && miso >= 0 && cs >= 0) {
            spi_mon_start(sck, mosi, miso, cs, 0);
        }
    }
    {
        int sda = pin_config_i2c_sda(), scl = pin_config_i2c_scl();
        if (sda >= 0 && scl >= 0) {
            i2c_mon_start(sda, scl, 0);
        }
    }
    buttons_init(on_button_event);
    tcp_server_start();
    http_status_start();
    dap_server_start();

    xTaskCreate(ui_task, "ui", 8192, NULL, 5, NULL);

    ESP_LOGI(TAG, "All subsystems initialized");
    ESP_LOGI(TAG, "HTTP: http://<ip>:80  TCP: tcp://<ip>:3333");
}
