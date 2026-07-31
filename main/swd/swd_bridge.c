#include "swd_bridge.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "pinout.h"
#include "pin_config.h"

static const char *TAG = "swd";

/* ---- SWD timing helper ----
 * Target ~1 MHz bit-bang: 1 us per half-clock.
 * esp_rom_delay_us is cycle-accurate and doesn't need a running FreeRTOS
 * tick, so we use it for consistent SWCLK timing.
 */
#include "esp_rom_sys.h"
#define SWD_HALF_CLK_US   5   /* 100 kHz */

/* ---- DP register addresses (APnDP=0 for DP, 1 for AP) ---- */
#define SWD_DP_IDCODE     0x0   /* R */
#define SWD_DP_ABORT      0x0   /* W */
#define SWD_DP_CTRL_STAT  0x4   /* R/W */
#define SWD_DP_SELECT     0x8   /* W */
#define SWD_DP_RDBUFF     0xC   /* R */

/* ---- Internal state ---- */
static bool s_swd_init = false;
static uint32_t s_current_ap_sel = 0;  /* cached DP SELECT for AP bank */

/* ================================================================ */
/*  Low-level GPIO bit-bang                                          */
/* ================================================================ */

static inline void swclk_high(void)  { gpio_set_level(pin_config_swclk(), 1); }
static inline void swclk_low(void)   { gpio_set_level(pin_config_swclk(), 0); }
static inline void swdio_high(void)  { gpio_set_level(pin_config_swdio(), 1); }
static inline void swdio_low(void)   { gpio_set_level(pin_config_swdio(), 0); }
static inline int  swdio_read(void)  { return gpio_get_level(pin_config_swdio()); }

static inline void swdio_dir_out(void)
{
    gpio_set_direction(pin_config_swdio(), GPIO_MODE_OUTPUT);
}

static inline void swdio_dir_in(void)
{
    gpio_set_direction(pin_config_swdio(), GPIO_MODE_INPUT);
}

/* Last ACK value read (diagnostic): 1=OK, 2=WAIT, 4=FAULT, 0/7=no-response */
static volatile uint32_t s_last_ack = 0xFF;

uint32_t swd_get_last_ack(void) { return s_last_ack; }

static inline void clock_pulse(void)
{
    swclk_low();
    esp_rom_delay_us(SWD_HALF_CLK_US);
    swclk_high();
    esp_rom_delay_us(SWD_HALF_CLK_US);
}

/* Write a single bit (MSB-first not used in SWD; SWD is LSB-first) */
static inline void swd_write_bit(int bit)
{
    if (bit) swdio_high(); else swdio_low();
    clock_pulse();
}

/* Read a single bit.
 * SWD timing: target drives data on falling edge of SWCLK,
 * host samples while SWCLK is low (before rising edge). */
static inline int swd_read_bit(void)
{
    swclk_low();
    esp_rom_delay_us(SWD_HALF_CLK_US);
    int val = swdio_read();
    swclk_high();
    esp_rom_delay_us(SWD_HALF_CLK_US);
    return val;
}

/* Write N bits, LSB-first */
static void swd_write_bits(uint32_t data, int nbits)
{
    swdio_dir_out();
    for (int i = 0; i < nbits; i++) {
        swd_write_bit((data >> i) & 1);
    }
}

/* Read N bits, LSB-first.
 * Caller must set SWDIO to input mode before calling and manage
 * direction afterwards. */
static uint32_t swd_read_bits(int nbits)
{
    uint32_t val = 0;
    for (int i = 0; i < nbits; i++) {
        if (swd_read_bit())
            val |= (1U << i);
    }
    return val;
}

/* ================================================================ */
/*  SWD protocol layer                                               */
/* ================================================================ */

/* Parity: XOR of all bits */
static int parity_odd(uint32_t val, int nbits)
{
    int p = 0;
    for (int i = 0; i < nbits; i++)
        p ^= (val >> i) & 1;
    return p;
}

/*
 * Perform an SWD transfer.
 *   apnp:    0 = DP, 1 = AP
 *   rnw:     0 = write, 1 = read
 *   addr:    register address (must be word-aligned; bits[3:2] used)
 *   data_in: data to write (for write ops)
 *   data_out: read data (for read ops)
 *
 * Returns ESP_OK on ACK=OK, ESP_ERR_TIMEOUT on WAIT/FAULT/no-response.
 */
static esp_err_t swd_transfer(bool apnp, bool rnw, uint8_t addr,
                               uint32_t data_in, uint32_t *data_out)
{
    /* Build 8-bit packet header:
     *  [Start=1][APnDP][RnW][A2][A3][Parity][Stop][Park]
     */
    uint8_t a2 = (addr >> 2) & 1;
    uint8_t a3 = (addr >> 3) & 1;
    uint8_t header = 0x81;   /* Start=1, Stop=0, Park=1 */
    if (apnp) header |= (1 << 1);
    if (rnw)  header |= (1 << 2);
    header |= (a2 << 3);
    header |= (a3 << 4);
    /* Parity over [APnDP, RnW, A2, A3] */
    int par = (apnp ? 1 : 0) ^ (rnw ? 1 : 0) ^ a2 ^ a3;
    if (par) header |= (1 << 5);

    /* Write header */
    swd_write_bits(header, 8);

    /* Turnaround: 1 idle clock (SWDIO as input) */
    swdio_dir_in();
    clock_pulse();

    /* Read 3-bit ACK (SWDIO already in input mode) */
    uint32_t ack = swd_read_bits(3);
    s_last_ack = ack;

    if (ack == 0x1) {
        /* OK ACK */
        if (rnw) {
            /* Read 32-bit data + parity (SWDIO stays input) */
            uint32_t data = swd_read_bits(32);
            int par_bit = swd_read_bit();
            swdio_dir_out();

            if (parity_odd(data, 32) != par_bit) {
                ESP_LOGE(TAG, "SWD read parity error (addr=0x%X)", addr);
                return ESP_ERR_INVALID_CRC;
            }
            if (data_out) *data_out = data;
        } else {
            /* Write 32-bit data + parity */
            swdio_dir_out();
            for (int i = 0; i < 32; i++) {
                swd_write_bit((data_in >> i) & 1);
            }
            int par = parity_odd(data_in, 32);
            swd_write_bit(par);
        }

        /* Turnaround: 1 idle clock */
        swdio_dir_in();
        clock_pulse();
        swdio_dir_out();

        /* Idle: 2 extra clocks with SWDIO high */
        swdio_high();
        clock_pulse();
        clock_pulse();

        return ESP_OK;
    } else if (ack == 0x2) {
        /* WAIT */
        ESP_LOGW(TAG, "SWD WAIT (addr=0x%X)", addr);
        swdio_dir_out();
        return ESP_ERR_TIMEOUT;
    } else {
        /* Fault (0x4) or no response (0x0) */
        ESP_LOGE(TAG, "SWD %s (addr=0x%X ack=0x%X)",
                 (ack == 0x4) ? "FAULT" : "NO-RESPONSE", addr, ack);
        swdio_dir_out();
        return ESP_FAIL;
    }
}

/* ================================================================ */
/*  Public SWD functions                                             */
/* ================================================================ */

esp_err_t swd_init(void)
{
    int io_swclk = pin_config_swclk();
    int io_swdio = pin_config_swdio();
    int io_nrst  = pin_config_nrst();

    /* SWCLK = output, default high (idle). Skip if not assigned (-1). */
    if (io_swclk >= 0) {
        gpio_config_t clk_cfg = {
            .pin_bit_mask = (1ULL << io_swclk),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&clk_cfg);
        gpio_set_level(io_swclk, 1);
    }

    /* SWDIO = push-pull bidirectional, start as output-high. */
    if (io_swdio >= 0) {
        gpio_config_t dio_cfg = {
            .pin_bit_mask = (1ULL << io_swdio),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&dio_cfg);
        gpio_set_level(io_swdio, 1);
    }

    /* NRST = output, default high (not in reset). */
    if (io_nrst >= 0) {
        gpio_config_t rst_cfg = {
            .pin_bit_mask = (1ULL << io_nrst),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&rst_cfg);
        gpio_set_level(io_nrst, 1);
    }

    s_swd_init = true;
    s_current_ap_sel = 0xFFFFFFFF;  /* force SELECT reload */
    ESP_LOGI(TAG, "SWD bridge ready: SWCLK=IO%d SWDIO=IO%d NRST=IO%d",
             io_swclk, io_swdio, io_nrst);
    return ESP_OK;
}

void swd_line_reset(void)
{
    swdio_dir_out();
    swdio_high();
    /* >50 SWCLK cycles with SWDIO high */
    for (int i = 0; i < 64; i++) {
        clock_pulse();
    }
}

void swd_jtag_to_swd(void)
{
    swdio_dir_out();
    /* JTAG-to-SWD selection sequence: 16-bit 0xE79E, sent LSB-first.
     * Per ARM ADI v5 spec: the wire sequence is 0111 1001 1110 0111.
     * 0xE79E LSB-first = 0,1,1,1,1,0,0,1, 1,1,1,0,0,1,1,1 ✓ */
    swd_write_bits(0xE79E, 16);
}

esp_err_t swd_read_idcode(uint32_t *idcode)
{
    if (!s_swd_init) {
        return ESP_ERR_INVALID_STATE;
    }
    return swd_transfer(false, true, SWD_DP_IDCODE, 0, idcode);
}

esp_err_t swd_read_dp(uint32_t addr, uint32_t *data)
{
    if (!s_swd_init) return ESP_ERR_INVALID_STATE;
    /* IDCODE is a special read that doesn't need SELECT */
    if (addr == SWD_DP_IDCODE) {
        return swd_transfer(false, true, addr, 0, data);
    }
    /* For RDBUFF: previous read result */
    return swd_transfer(false, true, addr, 0, data);
}

esp_err_t swd_write_dp(uint32_t addr, uint32_t data)
{
    if (!s_swd_init) return ESP_ERR_INVALID_STATE;
    return swd_transfer(false, false, addr, data, NULL);
}

/*
 * AP access requires SELECT register programming:
 *   SELECT[31:24] = APSEL
 *   SELECT[7:4]    = APBANKSEL
 *   SELECT[3:0]    = PBSEL
 * We keep it simple: assume APSEL=0 and set APBANKSEL from addr[7:4].
 */
static esp_err_t swd_select_ap_bank(uint32_t addr)
{
    uint32_t needed = (addr & 0xF0);  /* APBANKSEL bits */
    if (s_current_ap_sel == needed)
        return ESP_OK;

    esp_err_t err = swd_transfer(false, false, SWD_DP_SELECT, needed, NULL);
    if (err == ESP_OK) {
        s_current_ap_sel = needed;
    }
    return err;
}

esp_err_t swd_read_ap(uint32_t addr, uint32_t *data)
{
    if (!s_swd_init) return ESP_ERR_INVALID_STATE;

    esp_err_t err = swd_select_ap_bank(addr);
    if (err != ESP_OK) return err;

    /* AP read: first transfer initiates, result comes from RDBUFF */
    err = swd_transfer(true, true, addr, 0, NULL);
    if (err != ESP_OK) return err;

    /* Read result from DP RDBUFF */
    return swd_transfer(false, true, SWD_DP_RDBUFF, 0, data);
}

esp_err_t swd_write_ap(uint32_t addr, uint32_t data)
{
    if (!s_swd_init) return ESP_ERR_INVALID_STATE;

    esp_err_t err = swd_select_ap_bank(addr);
    if (err != ESP_OK) return err;

    return swd_transfer(true, false, addr, data, NULL);
}

esp_err_t swd_reset_target(bool assert)
{
    if (!s_swd_init) return ESP_ERR_INVALID_STATE;
    gpio_set_level(pin_config_nrst(), assert ? 0 : 1);
    ESP_LOGI(TAG, "Target %s", assert ? "reset asserted" : "released");
    return ESP_OK;
}

/* ================================================================ */
/*  JSON command handler                                            */
/* ================================================================ */

static uint32_t hex_to_u32(const char *hex)
{
    if (!hex) return 0;
    return (uint32_t)strtoul(hex, NULL, 16);
}

esp_err_t swd_bridge_handle(const char *cmd,
                            const char *addr_hex,
                            const char *data_hex,
                            char *resp, size_t resp_len)
{
    if (!cmd || !resp || resp_len == 0)
        return ESP_ERR_INVALID_ARG;

    if (strcmp(cmd, "reset") == 0) {
        swd_line_reset();
        swd_jtag_to_swd();
        swd_line_reset();
        uint32_t id = 0;
        esp_err_t err = swd_read_idcode(&id);
        if (err == ESP_OK) {
            snprintf(resp, resp_len, "{\"ok\":true,\"idcode\":\"0x%08lX\"}",
                     (unsigned long)id);
        } else {
            snprintf(resp, resp_len, "{\"ok\":false,\"err\":\"read_id failed\"}");
        }
        return ESP_OK;
    }

    if (strcmp(cmd, "read_id") == 0) {
        uint32_t id = 0;
        esp_err_t err = swd_read_idcode(&id);
        if (err == ESP_OK) {
            snprintf(resp, resp_len, "{\"ok\":true,\"idcode\":\"0x%08lX\"}",
                     (unsigned long)id);
        } else {
            snprintf(resp, resp_len, "{\"ok\":false,\"err\":\"%s\"}", esp_err_to_name(err));
        }
        return ESP_OK;
    }

    if (strcmp(cmd, "read_dp") == 0) {
        uint32_t addr = hex_to_u32(addr_hex);
        uint32_t data = 0;
        esp_err_t err = swd_read_dp(addr, &data);
        if (err == ESP_OK) {
            snprintf(resp, resp_len, "{\"ok\":true,\"data\":\"0x%08lX\"}",
                     (unsigned long)data);
        } else {
            snprintf(resp, resp_len, "{\"ok\":false,\"err\":\"%s\"}", esp_err_to_name(err));
        }
        return ESP_OK;
    }

    if (strcmp(cmd, "write_dp") == 0) {
        uint32_t addr = hex_to_u32(addr_hex);
        uint32_t data = hex_to_u32(data_hex);
        esp_err_t err = swd_write_dp(addr, data);
        if (err == ESP_OK) {
            snprintf(resp, resp_len, "{\"ok\":true}");
        } else {
            snprintf(resp, resp_len, "{\"ok\":false,\"err\":\"%s\"}", esp_err_to_name(err));
        }
        return ESP_OK;
    }

    if (strcmp(cmd, "read_ap") == 0) {
        uint32_t addr = hex_to_u32(addr_hex);
        uint32_t data = 0;
        esp_err_t err = swd_read_ap(addr, &data);
        if (err == ESP_OK) {
            snprintf(resp, resp_len, "{\"ok\":true,\"data\":\"0x%08lX\"}",
                     (unsigned long)data);
        } else {
            snprintf(resp, resp_len, "{\"ok\":false,\"err\":\"%s\"}", esp_err_to_name(err));
        }
        return ESP_OK;
    }

    if (strcmp(cmd, "write_ap") == 0) {
        uint32_t addr = hex_to_u32(addr_hex);
        uint32_t data = hex_to_u32(data_hex);
        esp_err_t err = swd_write_ap(addr, data);
        if (err == ESP_OK) {
            snprintf(resp, resp_len, "{\"ok\":true}");
        } else {
            snprintf(resp, resp_len, "{\"ok\":false,\"err\":\"%s\"}", esp_err_to_name(err));
        }
        return ESP_OK;
    }

    /* Unknown command */
    snprintf(resp, resp_len, "{\"ok\":false,\"err\":\"unknown cmd: %s\"}", cmd);
    return ESP_ERR_INVALID_ARG;
}
