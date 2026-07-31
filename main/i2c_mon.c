/*
 * i2c_mon.c - I2C bus monitor via dual RMT RX channels (hardware edge capture).
 *
 * I2C is a 2-signal protocol, so a single RMT channel can't decode it. We use
 * two RMT RX channels (SDA + SCL) at 1 MHz tick. Both capture edges with
 * timestamps into hardware buffers — no per-edge CPU ISR, so 100 kHz+ I2C is
 * captured reliably even while WiFi/HTTP/LCD run.
 *
 * A decode task merges the two edge streams by absolute time and decodes:
 *   - Start: SDA falls while SCL high
 *   - Stop:  SDA rises while SCL high
 *   - Data:  SCL rising edge samples SDA
 */
#include "i2c_mon.h"
#include "driver/gpio.h"
#include "driver/rmt_rx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"

static const char *TAG = "i2c";

static volatile bool s_running = false;
static i2c_mon_mode_t s_mode = I2C_MON_SLAVE;
static int s_pin_sda = -1, s_pin_scl = -1;
static volatile uint32_t s_isr_count = 0;
static uint8_t s_slave_addr = 0x50;

/* Transaction ring buffer */
static i2c_txn_t s_hist[I2C_MON_MAX];
static volatile int s_hist_max = I2C_MON_HISTORY;
static volatile int s_hist_w = 0;
static volatile int s_hist_n = 0;
static volatile uint32_t s_count = 0;

void i2c_mon_set_history_max(int max)
{
    if (max < 2) max = 2;
    if (max > I2C_MON_MAX) max = I2C_MON_MAX;
    s_hist_max = max;
    s_hist_w = 0;
    s_hist_n = 0;
}
int i2c_mon_get_history_max(void) { return s_hist_max; }

void i2c_mon_clear(void)
{
    s_hist_w = 0;
    s_hist_n = 0;
    s_count = 0;
    s_isr_count = 0;
}
int i2c_mon_get_hist_n(void) { return s_hist_n; }

/* Current transaction being assembled (decode-task context) */
static uint8_t s_cur_data[I2C_MON_MAX_LEN];
static uint8_t s_cur_byte = 0;
static int     s_bit_cnt = 0;
static int     s_byte_idx = 0;
static uint8_t s_addr_byte = 0;
static bool    s_is_read = false;
static bool    s_in_txn = false;
static uint8_t s_addr_mask = 0;

static void store_txn(void)
{
    if (s_byte_idx == 0) return;
    i2c_txn_t *t = &s_hist[s_hist_w];
    t->addr = s_addr_byte;
    t->read = s_is_read;
    t->seq  = s_count;
    t->len  = s_byte_idx;
    if (t->len > I2C_MON_MAX_LEN) t->len = I2C_MON_MAX_LEN;
    memcpy(t->data, s_cur_data, t->len);
    s_hist_w = (s_hist_w + 1) % s_hist_max;
    if (s_hist_n < s_hist_max) s_hist_n++;
    s_count++;
}

/* ---- I2C peripheral slave mode ---- */
#include "driver/i2c_slave.h"
static i2c_slave_dev_handle_t s_slave = NULL;

static void store_raw(uint8_t addr, bool read, const uint8_t *data, int len)
{
    i2c_txn_t *t = &s_hist[s_hist_w];
    t->addr = addr;
    t->read = read;
    t->seq = s_count;
    t->len = len > I2C_MON_MAX_LEN ? I2C_MON_MAX_LEN : len;
    if (t->len > 0) memcpy(t->data, data, t->len);
    s_hist_w = (s_hist_w + 1) % s_hist_max;
    if (s_hist_n < s_hist_max) s_hist_n++;
    s_count++;
}

static bool IRAM_ATTR slave_on_recv(i2c_slave_dev_handle_t h,
                                    const i2c_slave_rx_done_event_data_t *e, void *arg)
{
    (void)h; (void)arg;
    s_isr_count++;
    if (e->length > 0)
        store_raw(s_slave_addr << 1, false, e->buffer, e->length);
    return false;
}
static bool IRAM_ATTR slave_on_req(i2c_slave_dev_handle_t h,
                                   const i2c_slave_request_event_data_t *e, void *arg)
{
    (void)h; (void)e; (void)arg;
    s_isr_count++;
    store_raw(s_slave_addr << 1, true, NULL, 0);
    return false;
}

static void on_start(void)
{
    if (s_byte_idx > 0) store_txn();   /* repeated start */
    s_bit_cnt = s_byte_idx = 0;
    s_cur_byte = s_addr_byte = 0;
    s_is_read = false;
    s_in_txn = true;
}
static void on_stop(void)
{
    s_in_txn = false;
}
static void on_bit(int sda)
{
    int bit_in_byte = s_bit_cnt % 9;
    if (bit_in_byte < 8) {
        s_cur_byte = (s_cur_byte << 1) | sda;
    } else {
        if (s_bit_cnt == 8) {
            s_addr_byte = s_cur_byte;
            s_is_read = s_cur_byte & 1;
            s_cur_byte = 0;
        } else {
            if (s_addr_mask == 0 || (s_addr_byte >> 1) == s_addr_mask) {
                if (s_byte_idx < I2C_MON_MAX_LEN)
                    s_cur_data[s_byte_idx++] = s_cur_byte;
            }
            s_cur_byte = 0;
        }
        if (sda) { on_stop(); s_bit_cnt = 0; return; }   /* NACK */
    }
    s_bit_cnt++;
}

/* ---- RMT RX channels ---- */
#define RMT_BUF_SYMS  64
static rmt_channel_handle_t s_sda_ch, s_scl_ch;
static rmt_symbol_word_t s_sda_ring[RMT_BUF_SYMS], s_scl_ring[RMT_BUF_SYMS];
static int s_sda_n = 0, s_sda_cur = 0, s_scl_n = 0, s_scl_cur = 0;
static uint32_t s_sda_t = 0, s_scl_t = 0;   /* absolute time consumed */
static int s_sda_lvl = 1, s_scl_lvl = 1;    /* current level per stream */
static QueueHandle_t s_dec_q;

/* Merge the two streams up to the current buffer ends and decode. */
static void decode_merge(void)
{
    while (s_sda_cur < s_sda_n && s_scl_cur < s_scl_n) {
        const rmt_symbol_word_t *sd = &s_sda_ring[s_sda_cur % RMT_BUF_SYMS];
        const rmt_symbol_word_t *sc = &s_scl_ring[s_scl_cur % RMT_BUF_SYMS];
        uint32_t sda_dur = sd->duration0;
        uint32_t scl_dur = sc->duration0;
        uint32_t sda_end = s_sda_t + sda_dur;
        uint32_t scl_end = s_scl_t + scl_dur;
        if (sda_end <= scl_end) {
            /* SDA symbol ends first → SDA flips for next symbol */
            int old = s_sda_lvl;
            s_sda_lvl = !old;
            if (s_scl_lvl == 1) {            /* SCL high at this moment */
                if (old == 1 && s_sda_lvl == 0) on_start();
                else if (old == 0 && s_sda_lvl == 1) on_stop();
            }
            s_sda_t = sda_end;
            /* advance within the two-pulse symbol word */
            if (sd->duration1 == 0) { s_sda_cur++; s_sda_lvl = !(s_sda_lvl); }
            else {
                /* swap to the second pulse of this word */
                s_sda_ring[s_sda_cur % RMT_BUF_SYMS].duration0 = sd->duration1;
                s_sda_ring[s_sda_cur % RMT_BUF_SYMS].level0 = sd->level1;
                s_sda_ring[s_sda_cur % RMT_BUF_SYMS].duration1 = 0;
                s_sda_lvl = sd->level1;
            }
        } else {
            /* SCL symbol ends first → SCL flips */
            int old = s_scl_lvl;
            s_scl_lvl = !old;
            if (old == 0 && s_scl_lvl == 1 && s_in_txn) {  /* rising edge */
                on_bit(s_sda_lvl);
            }
            s_scl_t = scl_end;
            if (sc->duration1 == 0) { s_scl_cur++; s_scl_lvl = !(s_scl_lvl); }
            else {
                s_scl_ring[s_scl_cur % RMT_BUF_SYMS].duration0 = sc->duration1;
                s_scl_ring[s_scl_cur % RMT_BUF_SYMS].level0 = sc->level1;
                s_scl_ring[s_scl_cur % RMT_BUF_SYMS].duration1 = 0;
                s_scl_lvl = sc->level1;
            }
        }
    }
}

static void dec_task(void *arg)
{
    (void)arg;
    int chan;
    while (s_running) {
        if (xQueueReceive(s_dec_q, &chan, pdMS_TO_TICKS(100)) != pdTRUE) continue;
        decode_merge();
        /* compact consumed symbols */
        if (s_sda_cur > RMT_BUF_SYMS) { s_sda_cur -= RMT_BUF_SYMS; s_sda_n -= RMT_BUF_SYMS; }
        if (s_scl_cur > RMT_BUF_SYMS) { s_scl_cur -= RMT_BUF_SYMS; s_scl_n -= RMT_BUF_SYMS; }
    }
    vTaskDelete(NULL);
}

static bool IRAM_ATTR on_recv(rmt_channel_handle_t ch, const rmt_rx_done_event_data_t *e, void *arg)
{
    (void)ch; (void)arg;
    s_isr_count++;
    int chan = (intptr_t)arg;   /* 0=SDA, 1=SCL */
    rmt_symbol_word_t *ring = (chan == 0) ? s_sda_ring : s_scl_ring;
    int *n = (chan == 0) ? &s_sda_n : &s_scl_n;
    /* append received symbols into the per-channel ring (drop oldest if full) */
    for (int i = 0; i < e->num_symbols && i < RMT_BUF_SYMS; i++) {
        ring[(*n) % RMT_BUF_SYMS] = e->received_symbols[i];
        (*n)++;
    }
    /* re-arm for continuous reception */
    rmt_enable(ch);
    int q = chan;
    BaseType_t hp;
    xQueueSendFromISR(s_dec_q, &q, &hp);
    return hp == pdTRUE;
}

esp_err_t i2c_mon_start(int sda, int scl, uint8_t slave_addr)
{
    if (s_running) return ESP_ERR_INVALID_STATE;
    if (sda < 0 || scl < 0) return ESP_ERR_INVALID_ARG;

    s_slave_addr = (slave_addr == 0) ? 0x50 : slave_addr;
    s_pin_sda = sda; s_pin_scl = scl;
    s_count = 0; s_isr_count = 0;
    s_hist_w = s_hist_n = 0;
    memset(s_hist, 0, sizeof(s_hist));

    gpio_reset_pin(sda);
    gpio_reset_pin(scl);

    i2c_slave_config_t scfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .slave_addr = s_slave_addr,
        .addr_bit_len = I2C_ADDR_BIT_LEN_7,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .send_buf_depth = 64,
        .receive_buf_depth = 128,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t e = i2c_new_slave_device(&scfg, &s_slave);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_slave_device: %s", esp_err_to_name(e));
        return e;
    }
    i2c_slave_event_callbacks_t cbs = { .on_receive = slave_on_recv, .on_request = slave_on_req };
    i2c_slave_register_event_callbacks(s_slave, &cbs, NULL);

    s_mode = I2C_MON_SLAVE;
    s_running = true;
    ESP_LOGI(TAG, "I2C SLAVE @0x%02X on SDA=IO%d SCL=IO%d", s_slave_addr, sda, scl);
    return ESP_OK;
}

esp_err_t i2c_mon_start_passive(int sda, int scl)
{
    if (s_running) return ESP_ERR_INVALID_STATE;
    if (sda < 0 || scl < 0) return ESP_ERR_INVALID_ARG;

    s_pin_sda = sda; s_pin_scl = scl; s_addr_mask = 0;
    s_bit_cnt = s_byte_idx = 0;
    s_cur_byte = s_addr_byte = 0;
    s_is_read = s_in_txn = false;
    s_count = 0; s_isr_count = 0;
    s_hist_w = s_hist_n = 0;
    s_sda_n = s_sda_cur = s_scl_n = s_scl_cur = 0;
    s_sda_t = s_scl_t = 0;
    s_sda_lvl = s_scl_lvl = 1;
    memset(s_hist, 0, sizeof(s_hist));

    gpio_reset_pin(sda);
    gpio_reset_pin(scl);
    gpio_config_t cfg = {
        .pin_bit_mask = 0,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    cfg.pin_bit_mask = 1ULL << sda; gpio_config(&cfg);
    cfg.pin_bit_mask = 1ULL << scl; gpio_config(&cfg);

    s_dec_q = xQueueCreate(8, sizeof(int));

    rmt_rx_channel_config_t rx_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 1 * 1000 * 1000,   /* 1 MHz = 1 us tick */
        .mem_block_symbols = RMT_BUF_SYMS,
    };
    rmt_rx_event_callbacks_t cbs = { .on_recv_done = on_recv };

    rx_cfg.gpio_num = sda;
    esp_err_t e = rmt_new_rx_channel(&rx_cfg, &s_sda_ch);
    if (e != ESP_OK) return e;
    rmt_rx_register_event_callbacks(s_sda_ch, &cbs, (void *)(intptr_t)0);
    rmt_enable(s_sda_ch);

    rx_cfg.gpio_num = scl;
    e = rmt_new_rx_channel(&rx_cfg, &s_scl_ch);
    if (e != ESP_OK) return e;
    rmt_rx_register_event_callbacks(s_scl_ch, &cbs, (void *)(intptr_t)1);
    rmt_enable(s_scl_ch);

    s_mode = I2C_MON_PASSIVE;
    s_running = true;
    xTaskCreate(dec_task, "i2c_dec", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "I2C PASSIVE (RMT) on SDA=IO%d SCL=IO%d", sda, scl);
    return ESP_OK;
}

void i2c_mon_stop(void)
{
    if (!s_running) return;
    s_running = false;
    vTaskDelay(pdMS_TO_TICKS(50));
    if (s_mode == I2C_MON_SLAVE) {
        if (s_slave) { i2c_del_slave_device(s_slave); s_slave = NULL; }
    } else {
        rmt_disable(s_sda_ch);
        rmt_disable(s_scl_ch);
        rmt_del_channel(s_sda_ch);
        rmt_del_channel(s_scl_ch);
        s_sda_ch = s_scl_ch = NULL;
        if (s_dec_q) { vQueueDelete(s_dec_q); s_dec_q = NULL; }
    }
    gpio_reset_pin(s_pin_sda);
    gpio_reset_pin(s_pin_scl);
    s_pin_sda = s_pin_scl = -1;
    ESP_LOGI(TAG, "I2C monitor stopped");
}

bool i2c_mon_running(void)       { return s_running; }
i2c_mon_mode_t i2c_mon_mode(void) { return s_mode; }
uint32_t i2c_mon_get_count(void) { return s_count; }
uint32_t i2c_mon_get_isr_count(void) { return s_isr_count; }

int i2c_mon_get_history(i2c_txn_t *out, int max)
{
    int n = s_hist_n;
    if (n > max) n = max;
    int start = (s_hist_w - n + s_hist_max) % s_hist_max;
    for (int i = 0; i < n; i++)
        out[i] = s_hist[(start + i) % s_hist_max];
    return n;
}

/* ========== I2C Master Send ========== */
#include "driver/i2c_master.h"

static i2c_master_bus_handle_t s_i2c_bus = NULL;
static i2c_master_dev_handle_t s_i2c_dev = NULL;
static bool s_send_running = false;
static uint8_t s_dev_addr = 0;

esp_err_t i2c_send_start(int sda, int scl, int clock_hz)
{
    if (s_send_running) i2c_send_stop();
    if (sda < 0 || scl < 0) return ESP_ERR_INVALID_ARG;

    gpio_reset_pin(sda);
    gpio_reset_pin(scl);

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t e = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus: %s", esp_err_to_name(e));
        return e;
    }
    s_send_running = true;
    ESP_LOGI(TAG, "I2C master on SDA=IO%d SCL=IO%d clk=%d", sda, scl, clock_hz);
    return ESP_OK;
}

static esp_err_t ensure_i2c_dev(uint8_t addr)
{
    if (s_i2c_dev && s_dev_addr == addr) return ESP_OK;
    if (s_i2c_dev) {
        i2c_master_bus_rm_device(s_i2c_dev);
        s_i2c_dev = NULL;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400000,
    };
    esp_err_t e = i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_i2c_dev);
    if (e == ESP_OK) s_dev_addr = addr;
    return e;
}

esp_err_t i2c_write(uint8_t addr, const uint8_t *data, int len)
{
    if (!s_send_running) return ESP_ERR_INVALID_STATE;
    esp_err_t e = ensure_i2c_dev(addr);
    if (e != ESP_OK) return e;
    return i2c_master_transmit(s_i2c_dev, (uint8_t *)data, len, 1000);
}

esp_err_t i2c_read(uint8_t addr, uint8_t *buf, int len)
{
    if (!s_send_running) return ESP_ERR_INVALID_STATE;
    esp_err_t e = ensure_i2c_dev(addr);
    if (e != ESP_OK) return e;
    return i2c_master_receive(s_i2c_dev, buf, len, 1000);
}

esp_err_t i2c_write_read(uint8_t addr, const uint8_t *wdata, int wlen,
                         uint8_t *rbuf, int rlen)
{
    if (!s_send_running) return ESP_ERR_INVALID_STATE;
    esp_err_t e = ensure_i2c_dev(addr);
    if (e != ESP_OK) return e;
    return i2c_master_transmit_receive(s_i2c_dev, (uint8_t *)wdata, wlen,
                                       rbuf, rlen, 1000);
}

void i2c_send_stop(void)
{
    if (!s_send_running) return;
    if (s_i2c_dev) {
        i2c_master_bus_rm_device(s_i2c_dev);
        s_i2c_dev = NULL;
    }
    if (s_i2c_bus) {
        i2c_del_master_bus(s_i2c_bus);
        s_i2c_bus = NULL;
    }
    s_send_running = false;
    ESP_LOGI(TAG, "I2C master stopped");
}

bool i2c_send_running(void) { return s_send_running; }
