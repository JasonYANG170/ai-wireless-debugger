#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SPI_MON_MAX_LEN  64
#define SPI_MON_MAX      100   /* compile-time cap of the history ring */
#define SPI_MON_HISTORY  50    /* default effective history depth */

/* One captured SPI transaction. */
typedef struct {
    uint8_t mosi[SPI_MON_MAX_LEN];   /* master -> slave bytes */
    uint8_t miso[SPI_MON_MAX_LEN];   /* slave -> master bytes (0 if unused) */
    int     len;                      /* byte count */
    uint32_t seq;                     /* monotonic sequence number */
} spi_txn_t;

/* Start SPI3 slave capture on the given pins (DMA, mode 0..3).
 * The ESP is the slave; each completed transaction is stored in the history
 * ring buffer. Both MOSI (master→slave) and MISO (slave→master) are captured. */
esp_err_t spi_mon_start(int sck, int mosi, int miso, int cs, int mode);

/* Stop the SPI slave and free the bus. */
void spi_mon_stop(void);

bool spi_mon_running(void);

/* Total transactions captured since start. */
uint32_t spi_mon_get_count(void);

/* Total transmit timeouts (no master activity) — diagnostic. */
uint32_t spi_mon_get_timeouts(void);

/* Read a transaction from the history ring buffer by absolute index.
 * idx=0 is the oldest stored, idx=count-1 is the newest.
 * Returns the byte count (0 if index is out of range). */
int spi_mon_read_history(uint32_t seq, spi_txn_t *out);

/* Copy the latest N transactions into `out` (up to max).
 * Returns how many were actually copied. */
int spi_mon_get_history(spi_txn_t *out, int max);

/* Set the effective history depth (10..SPI_MON_MAX). Clears current history. */
void spi_mon_set_history_max(int max);
int  spi_mon_get_history_max(void);

/* Clear the captured history (keep running). */
void spi_mon_clear(void);

/* Number of transactions currently in the ring (0..max). */
int spi_mon_get_hist_n(void);

/* ---- SPI Master Send ---- */

/* Initialize SPI master on the given pins (mode 0..3, clock Hz).
 * Can coexist with the slave monitor if on different pins, or use the same
 * pins in shared mode (slave stopped first). */
esp_err_t spi_send_start(int sck, int mosi, int miso, int cs, int mode, int clock_hz);

/* Send `len` bytes via SPI master. `rx` can be NULL if you don't need MISO. */
esp_err_t spi_send_bytes(const uint8_t *tx, uint8_t *rx, int len);

/* Release the SPI master bus. */
void spi_send_stop(void);

bool spi_send_running(void);

#ifdef __cplusplus
}
#endif
