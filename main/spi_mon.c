/*
 * spi_mon.c - SPI slave capture with history ring buffer (SPI3_HOST + DMA).
 *
 * Each completed SPI transaction stores both MOSI (master->slave) and MISO
 * (slave->master) bytes in a ring buffer. The LCD SPI page and web UI read
 * this history to display scrolling lines with direction markers (>> / <<).
 */
#include "spi_mon.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_slave.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "spi";

static volatile bool     s_running = false;
static TaskHandle_t      s_task    = NULL;
static volatile uint32_t s_count   = 0;
static volatile uint32_t s_timeouts = 0;   /* diagnostic: transmit timeouts */

/* History ring buffer */
static spi_txn_t s_hist[SPI_MON_MAX];
static volatile int s_hist_max = SPI_MON_HISTORY;   /* effective depth */
static int       s_hist_w = 0;
static int       s_hist_n = 0;

void spi_mon_set_history_max(int max)
{
    if (max < 2) max = 2;
    if (max > SPI_MON_MAX) max = SPI_MON_MAX;
    s_hist_max = max;
    s_hist_w = 0;
    s_hist_n = 0;
}
int spi_mon_get_history_max(void) { return s_hist_max; }

void spi_mon_clear(void)
{
    s_hist_w = 0;
    s_hist_n = 0;
    s_count = 0;
}
int spi_mon_get_hist_n(void) { return s_hist_n; }

static void store_txn(const uint8_t *mosi, const uint8_t *miso, int len)
{
    spi_txn_t *t = &s_hist[s_hist_w];
    t->len = len;
    t->seq = s_count;
    memcpy(t->mosi, mosi, len);
    if (miso) memcpy(t->miso, miso, len);
    else      memset(t->miso, 0, len);
    s_hist_w = (s_hist_w + 1) % s_hist_max;
    if (s_hist_n < s_hist_max) s_hist_n++;
    s_count++;
}

static void spi_task(void *arg)
{
    (void)arg;
    uint8_t rx_buf[SPI_MON_MAX_LEN];
    uint8_t tx_buf[SPI_MON_MAX_LEN];
    memset(tx_buf, 0, sizeof(tx_buf));

    while (s_running) {
        spi_slave_transaction_t t = {0};
        t.length    = sizeof(rx_buf) * 8;
        t.rx_buffer = rx_buf;
        t.tx_buffer = tx_buf;
        esp_err_t e = spi_slave_transmit(SPI3_HOST, &t, pdMS_TO_TICKS(200));
        if (!s_running) break;
        if (e != ESP_OK) { s_timeouts++; continue; }
        int n = t.trans_len / 8;
        if (n > (int)sizeof(rx_buf)) n = (int)sizeof(rx_buf);
        if (n > 0) store_txn(rx_buf, tx_buf, n);
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t spi_mon_start(int sck, int mosi, int miso, int cs, int mode)
{
    if (s_running) return ESP_ERR_INVALID_STATE;
    if (sck < 0 || mosi < 0 || miso < 0 || cs < 0) return ESP_ERR_INVALID_ARG;

    /* Explicitly reset the GPIO pins before SPI configuration.
     * Previous boot may have left them in SWD/UART mode (e.g. IO13 as SWD
     * SWCLK output), which prevents SPI3 from reconfiguring them as inputs.
     * gpio_reset_pin clears the mode and GPIO matrix function. */
    gpio_reset_pin(sck);
    gpio_reset_pin(mosi);
    gpio_reset_pin(miso);
    gpio_reset_pin(cs);

    spi_bus_config_t buscfg = {
        .mosi_io_num  = mosi,
        .miso_io_num  = miso,
        .sclk_io_num  = sck,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = SPI_MON_MAX_LEN,
    };
    spi_slave_interface_config_t slvcfg = {
        .spics_io_num = cs,
        .flags        = 0,
        .queue_size   = 2,
        .mode         = mode & 3,
        .post_setup_cb = NULL,
        .post_trans_cb = NULL,
    };
    esp_err_t e = spi_slave_initialize(SPI3_HOST, &buscfg, &slvcfg, SPI_DMA_CH_AUTO);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "spi_slave_initialize: %s", esp_err_to_name(e));
        return e;
    }
    s_count = 0;
    s_hist_w = 0;
    s_hist_n = 0;
    memset(s_hist, 0, sizeof(s_hist));
    s_running = true;
    xTaskCreate(spi_task, "spi_mon", 4096, NULL, 5, &s_task);
    ESP_LOGI(TAG, "SPI slave on SPI3: SCK=IO%d MOSI=IO%d MISO=IO%d CS=IO%d mode=%d",
             sck, mosi, miso, cs, mode);
    return ESP_OK;
}

void spi_mon_stop(void)
{
    if (!s_running) return;
    s_running = false;
    vTaskDelay(pdMS_TO_TICKS(260));
    spi_slave_free(SPI3_HOST);
}

bool spi_mon_running(void) { return s_running; }
uint32_t spi_mon_get_count(void) { return s_count; }
uint32_t spi_mon_get_timeouts(void) { return s_timeouts; }

int spi_mon_get_history(spi_txn_t *out, int max)
{
    int n = s_hist_n;
    if (n > max) n = max;
    int start = (s_hist_w - n + s_hist_max) % s_hist_max;
    for (int i = 0; i < n; i++)
        out[i] = s_hist[(start + i) % s_hist_max];
    return n;
}

/* ========== SPI Master Send ========== */
#include "driver/spi_master.h"

static spi_device_handle_t s_spi_dev = NULL;
static bool s_send_running = false;

esp_err_t spi_send_start(int sck, int mosi, int miso, int cs, int mode, int clock_hz)
{
    if (s_send_running) spi_send_stop();
    if (sck < 0 || mosi < 0 || cs < 0) return ESP_ERR_INVALID_ARG;

    gpio_reset_pin(sck);
    gpio_reset_pin(mosi);
    if (miso >= 0) gpio_reset_pin(miso);
    gpio_reset_pin(cs);

    spi_bus_config_t buscfg = {
        .mosi_io_num  = mosi,
        .miso_io_num  = miso,
        .sclk_io_num  = sck,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    esp_err_t e = spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(e));
        return e;
    }

    spi_device_interface_config_t devcfg = {
        .mode = mode & 3,
        .clock_speed_hz = clock_hz > 0 ? clock_hz : 1000000,
        .spics_io_num = cs,
        .queue_size = 1,
    };
    e = spi_bus_add_device(SPI3_HOST, &devcfg, &s_spi_dev);
    if (e != ESP_OK) {
        spi_bus_free(SPI3_HOST);
        ESP_LOGE(TAG, "spi_bus_add_device: %s", esp_err_to_name(e));
        return e;
    }
    s_send_running = true;
    ESP_LOGI(TAG, "SPI master on SPI2: SCK=IO%d MOSI=IO%d MISO=IO%d CS=IO%d mode=%d clk=%d",
             sck, mosi, miso, cs, mode, clock_hz);
    return ESP_OK;
}

esp_err_t spi_send_bytes(const uint8_t *tx, uint8_t *rx, int len)
{
    if (!s_send_running || !s_spi_dev) return ESP_ERR_INVALID_STATE;
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    return spi_device_transmit(s_spi_dev, &t);
}

void spi_send_stop(void)
{
    if (!s_send_running) return;
    if (s_spi_dev) {
        spi_bus_remove_device(s_spi_dev);
        s_spi_dev = NULL;
    }
    spi_bus_free(SPI3_HOST);
    s_send_running = false;
    ESP_LOGI(TAG, "SPI master stopped");
}

bool spi_send_running(void) { return s_send_running; }
