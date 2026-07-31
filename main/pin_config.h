#pragma once
#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pin kinds shown on the SWD / J3 diagram. 5V/GND/DUT are power/reference
 * (fixed); the other five are signal functions that can be permuted over the
 * five fixed J3 signal IOs. */
typedef enum {
    PIN_5V = 0,
    PIN_GND,
    PIN_DUT,
    PIN_SWCLK,
    PIN_SWDIO,
    PIN_NRST,
    PIN_TX,
    PIN_RX,
    PIN_PWM,
    PIN_SPI_SCK,
    PIN_SPI_MOSI,
    PIN_SPI_MISO,
    PIN_SPI_CS,
    PIN_I2C_SDA,
    PIN_I2C_SCL,
    PIN_KIND_COUNT
} pin_kind_t;

#define PIN_NUM_SIGNAL_SLOTS 5

/* The five J3 signal positions are physically routed (via the TXB0106 level
 * shifter) to fixed ESP GPIOs. Pin reconfiguration = permuting the five signal
 * functions over these five fixed IOs (the IOs themselves never change). */

/* Load persisted config from NVS (defaults if none). Call once after
 * nvs_flash_init(), BEFORE serial_bridge_init() and swd_init(). */
void pin_config_init(void);

/* Diagram position -> the function assigned there / the fixed GPIO there. */
pin_kind_t pin_config_pos_func(int pos);
int        pin_config_pos_io(int pos);

/* The ESP GPIO currently carrying `func` (derived from the position it's
 * assigned to). */
int pin_config_func_io(pin_kind_t func);

/* Convenience: current GPIO for each SWD / UART signal. */
int pin_config_swclk(void);
int pin_config_swdio(void);
int pin_config_nrst(void);
int pin_config_tx(void);
int pin_config_rx(void);
int pin_config_pwm(void);
int pin_config_spi_sck(void);
int pin_config_spi_mosi(void);
int pin_config_spi_miso(void);
int pin_config_spi_cs(void);
int pin_config_i2c_sda(void);
int pin_config_i2c_scl(void);

/* Set the function-per-position order (must be a permutation of the five
 * signal kinds). Saves to NVS. */
esp_err_t pin_config_set_signal_order(const pin_kind_t order[PIN_NUM_SIGNAL_SLOTS]);

const char *pin_config_kind_label(pin_kind_t k);

#ifdef __cplusplus
}
#endif
