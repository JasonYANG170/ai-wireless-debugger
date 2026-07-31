#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define I2C_MON_MAX_LEN   64
#define I2C_MON_MAX       100   /* compile-time cap */
#define I2C_MON_HISTORY   50    /* default effective depth */

/* One captured I2C transaction (address byte + data bytes). */
typedef struct {
    uint8_t data[I2C_MON_MAX_LEN]; /* data bytes (excluding address) */
    uint8_t addr;                   /* 7-bit slave address (raw byte) */
    int     len;                    /* number of data bytes */
    bool    read;                   /* true=master-read, false=master-write */
    uint32_t seq;                   /* monotonic sequence number */
} i2c_txn_t;

/* I2C monitor mode: slave (act as target, reliable capture) or passive
 * (RMT listen, no ACK — needs a real slave on the bus). */
typedef enum { I2C_MON_SLAVE = 0, I2C_MON_PASSIVE } i2c_mon_mode_t;

/* Start I2C monitor. Default = SLAVE mode at `slave_addr` (0 → 0x50): the
 * debugger ACKs the master and captures writes reliably. */
esp_err_t i2c_mon_start(int sda, int scl, uint8_t slave_addr);

/* Restart in passive (RMT listen) mode — sniff an external master+slave pair. */
esp_err_t i2c_mon_start_passive(int sda, int scl);

/* Current mode. */
i2c_mon_mode_t i2c_mon_mode(void);

/* Stop the I2C monitor and release the GPIO pins. */
void i2c_mon_stop(void);

bool i2c_mon_running(void);

/* Total transactions captured since start. */
uint32_t i2c_mon_get_count(void);

/* ISR invocation count (diagnostic). */
uint32_t i2c_mon_get_isr_count(void);

/* Copy the latest N transactions into `out` (up to max).
 * Returns how many were actually copied. */
int i2c_mon_get_history(i2c_txn_t *out, int max);

/* Set the effective history depth (10..I2C_MON_MAX). Clears current history. */
void i2c_mon_set_history_max(int max);
int  i2c_mon_get_history_max(void);

/* Clear the captured history (keep running). */
void i2c_mon_clear(void);

/* Number of transactions currently in the ring (0..max). */
int i2c_mon_get_hist_n(void);

/* ---- I2C Master Send ---- */

/* Initialize I2C master on the given pins. */
esp_err_t i2c_send_start(int sda, int scl, int clock_hz);

/* Write `len` bytes to slave at `addr` (7-bit address). */
esp_err_t i2c_write(uint8_t addr, const uint8_t *data, int len);

/* Read `len` bytes from slave at `addr` into `buf`. */
esp_err_t i2c_read(uint8_t addr, uint8_t *buf, int len);

/* Write `wlen` bytes then read `rlen` bytes (common register-read pattern). */
esp_err_t i2c_write_read(uint8_t addr, const uint8_t *wdata, int wlen,
                         uint8_t *rbuf, int rlen);

/* Release the I2C master bus. */
void i2c_send_stop(void);

bool i2c_send_running(void);

#ifdef __cplusplus
}
#endif
