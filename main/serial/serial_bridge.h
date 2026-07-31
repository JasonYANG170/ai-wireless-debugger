#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Configuration ---- */
#define SERIAL_BUF_SIZE        2048   /* internal stream buffer size */
#define SERIAL_BAUD_DEFAULT    115200
#define SERIAL_RX_BUF_BYTES    2048   /* UART driver RX buffer */
#define SERIAL_TX_BUF_BYTES    0     /* UART driver TX buffer (0 = blocking) */
#define SERIAL_STACK_SIZE      4096
#define SERIAL_TASK_PRIO       8

/* ---- Callback ---- */
typedef void (*serial_rx_cb_t)(const uint8_t *data, size_t len);

/* ---- API ---- */

/* Initialise UART1 (TX=47, RX=21), install driver, start event task. */
esp_err_t serial_bridge_init(uint32_t baudrate);

/* Read bridged data from internal stream buffer. Returns bytes read. */
size_t serial_bridge_read(uint8_t *buf, size_t max_len, uint32_t timeout_ms);

/* Write data to the DUT via UART1. Returns bytes written. */
size_t serial_bridge_write(const uint8_t *buf, size_t len);

/* Change baud rate at runtime. */
esp_err_t serial_bridge_set_baud(uint32_t baudrate);

/* Recreate the RX stream buffer with a new size (bytes). Buffered data is lost. */
esp_err_t serial_bridge_set_bufsize(size_t bytes);

/* Drain/discard all buffered RX data. */
void serial_bridge_clear(void);

/* Bytes currently buffered; configured buffer capacity (bytes). */
size_t serial_bridge_get_buffered(void);
size_t serial_bridge_get_bufsize_current(void);

/* Read from the independent MCP stream (doesn't compete with web/TCP). */
size_t serial_bridge_read_mcp(uint8_t *buf, size_t maxlen, uint32_t timeout_ms);

/* Re-apply the UART TX/RX pins from pin_config (call after reassigning the
 * signal mapping). */
esp_err_t serial_bridge_repin(void);

/* Get running RX / TX byte counters (atomic). */
uint32_t serial_bridge_get_rx_count(void);
uint32_t serial_bridge_get_tx_count(void);

/* Set a callback invoked when data arrives from the DUT. */
void serial_bridge_set_rx_callback(serial_rx_cb_t cb);

#ifdef __cplusplus
}
#endif
