#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Menu page IDs */
typedef enum {
    MENU_HOME = 0,    /* icon grid: Status / RX Mon / SWD / Config */
    MENU_STATUS,      /* status page: WiFi/IP/baud/counters */
    MENU_RX_MON,      /* live RX data monitor */
    MENU_SWD,         /* SWD debug page */
    MENU_CONFIG,      /* config page */
    MENU_PWM,         /* PWM monitor: frequency + duty cycle */
    MENU_SPI,         /* SPI slave monitor: captured bytes */
    MENU_I2C,         /* I2C bus monitor: captured transactions */
    MENU_AI,          /* AI assistant page */
    MENU_COUNT
} menu_page_t;

/* Initialize the menu UI system. Call after lcd_init(). */
void menu_init(void);

/* Navigation input from buttons */
void menu_on_sw1_press(void);       /* prev item (home) or no-op (subpage) */
void menu_on_sw3_press(void);       /* next item (home) or no-op (subpage) */
void menu_on_sw2_press(void);       /* enter subpage (from home) */
void menu_on_sw2_long_press(void);  /* exit subpage -> back to home */
void menu_on_sw2_hold(void);        /* hold SW2 (e.g. I2C mode toggle) */

/* Feed serial RX data to the RX monitor page */
void menu_push_rx_data(const uint8_t *data, uint32_t len);

/* Echo serial TX data (e.g. sent from the web UI) into the RX monitor page,
 * prefixed with '<' so it is distinguishable from received ('>') data. */
void menu_push_tx_data(const uint8_t *data, uint32_t len);

/* Simulate button press from web API: 1=SW1, 2=SW2, 3=SW3.
 * action: "press" or "long" */
void menu_simulate_button(int btn_id, const char *action);

/* Render the current page. Call periodically (e.g. 5Hz from UI task). */
void menu_render(void);

#ifdef __cplusplus
}
#endif
