/*
 * pin_config.c - NVS-backed protocol assignment for the 5 fixed J3 signal IOs.
 *
 * The J3 connector has 5 signal pins, each hard-wired (via the TXB0106 level
 * shifter) to a fixed ESP GPIO:
 *     pos0 (row1 col2) -> IO13
 *     pos1 (row1 col3) -> IO21
 *     pos2 (row2 col1) -> IO12
 *     pos3 (row2 col2) -> IO14
 *     pos4 (row2 col3) -> IO47
 *
 * Configuration permutes the 5 signal functions (SWCLK, SWDIO, NRST, TX, RX)
 * over these 5 positions. Both the SWD bit-bang and the UART driver read the
 * resulting function->GPIO mapping, so reassigning a position (e.g. putting
 * RX/TX on IO13/IO14) takes effect in hardware.
 */
#include "pin_config.h"
#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "pinconf";

#define NVS_NS    "pinconf"
#define KEY_ORDER "order"

/* Fixed PCB routing: diagram signal position -> ESP GPIO. */
static const int POS_IO[PIN_NUM_SIGNAL_SLOTS] = { 13, 21, 12, 14, 47 };

/* Default function-per-position. SWD removed from selectable set. */
static const pin_kind_t DEFAULT_ORDER[PIN_NUM_SIGNAL_SLOTS] = {
    PIN_TX, PIN_RX, PIN_PWM, PIN_I2C_SDA, PIN_I2C_SCL,
};

static pin_kind_t s_order[PIN_NUM_SIGNAL_SLOTS];

static bool is_signal_kind(pin_kind_t k)
{
    return k == PIN_TX    || k == PIN_RX    || k == PIN_PWM    ||
           k == PIN_SPI_SCK || k == PIN_SPI_MOSI ||
           k == PIN_SPI_MISO || k == PIN_SPI_CS ||
           k == PIN_I2C_SDA || k == PIN_I2C_SCL;
}

static bool order_is_valid(const pin_kind_t order[PIN_NUM_SIGNAL_SLOTS])
{
    bool seen[PIN_KIND_COUNT] = { false };
    for (int i = 0; i < PIN_NUM_SIGNAL_SLOTS; i++) {
        if (!is_signal_kind(order[i]) || seen[order[i]]) return false;
        seen[order[i]] = true;
    }
    return true;
}

void pin_config_init(void)
{
    memcpy(s_order, DEFAULT_ORDER, sizeof(s_order));

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no saved pin config; using defaults");
        return;
    }
    size_t len = sizeof(s_order);
    if (nvs_get_blob(h, KEY_ORDER, s_order, &len) != ESP_OK || len != sizeof(s_order)) {
        memcpy(s_order, DEFAULT_ORDER, sizeof(s_order));
    }
    nvs_close(h);

    if (!order_is_valid(s_order)) {
        ESP_LOGW(TAG, "saved order invalid, reverting to defaults");
        memcpy(s_order, DEFAULT_ORDER, sizeof(s_order));
    }
    ESP_LOGI(TAG, "signal map: IO%d=SWCLK IO%d=SWDIO IO%d=NRST IO%d=TX IO%d=RX",
             pin_config_swclk(), pin_config_swdio(), pin_config_nrst(),
             pin_config_tx(), pin_config_rx());
}

static esp_err_t save_order(void)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_blob(h, KEY_ORDER, s_order, sizeof(s_order));
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

pin_kind_t pin_config_pos_func(int pos)
{
    if (pos < 0 || pos >= PIN_NUM_SIGNAL_SLOTS) return PIN_KIND_COUNT;
    return s_order[pos];
}

int pin_config_pos_io(int pos)
{
    if (pos < 0 || pos >= PIN_NUM_SIGNAL_SLOTS) return -1;
    return POS_IO[pos];
}

int pin_config_func_io(pin_kind_t func)
{
    for (int p = 0; p < PIN_NUM_SIGNAL_SLOTS; p++) {
        if (s_order[p] == func) return POS_IO[p];
    }
    return -1;
}

int pin_config_swclk(void) { return pin_config_func_io(PIN_SWCLK); }
int pin_config_swdio(void) { return pin_config_func_io(PIN_SWDIO); }
int pin_config_nrst(void)  { return pin_config_func_io(PIN_NRST); }
int pin_config_tx(void)    { return pin_config_func_io(PIN_TX); }
int pin_config_rx(void)    { return pin_config_func_io(PIN_RX); }
int pin_config_pwm(void)      { return pin_config_func_io(PIN_PWM); }
int pin_config_spi_sck(void)  { return pin_config_func_io(PIN_SPI_SCK); }
int pin_config_spi_mosi(void) { return pin_config_func_io(PIN_SPI_MOSI); }
int pin_config_spi_miso(void) { return pin_config_func_io(PIN_SPI_MISO); }
int pin_config_spi_cs(void)   { return pin_config_func_io(PIN_SPI_CS); }
int pin_config_i2c_sda(void)  { return pin_config_func_io(PIN_I2C_SDA); }
int pin_config_i2c_scl(void)  { return pin_config_func_io(PIN_I2C_SCL); }

esp_err_t pin_config_set_signal_order(const pin_kind_t order[PIN_NUM_SIGNAL_SLOTS])
{
    if (!order_is_valid(order)) {
        ESP_LOGW(TAG, "invalid signal order");
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(s_order, order, sizeof(s_order));
    return save_order();
}

const char *pin_config_kind_label(pin_kind_t k)
{
    switch (k) {
    case PIN_5V:    return "5V";
    case PIN_GND:   return "GND";
    case PIN_DUT:   return "DUT";
    case PIN_SWCLK: return "SWCLK";
    case PIN_SWDIO: return "SWDIO";
    case PIN_NRST:  return "NRST";
    case PIN_TX:    return "TX";
    case PIN_RX:    return "RX";
    case PIN_PWM:   return "PWM";
    case PIN_SPI_SCK:  return "SCK";
    case PIN_SPI_MOSI: return "MOSI";
    case PIN_SPI_MISO: return "MISO";
    case PIN_SPI_CS:   return "CS";
    case PIN_I2C_SDA:  return "SDA";
    case PIN_I2C_SCL:  return "SCL";
    default:        return "?";
    }
}
