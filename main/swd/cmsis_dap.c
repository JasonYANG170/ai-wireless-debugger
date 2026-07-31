/*
 * cmsis_dap.c - CMSIS-DAP v1 protocol handler for OpenOCD/pyOCD.
 *
 * Translates CMSIS-DAP commands into the existing GPIO bit-bang SWD calls.
 * Designed to be called from a TCP server (one command per packet).
 */
#include "cmsis_dap.h"
#include "swd_bridge.h"
#include <string.h>
#include "esp_log.h"

static const char *TAG = "dap";

static bool s_connected = false;
static uint32_t s_clock_hz = 1000000;  /* placeholder, not used by bit-bang */

void cmsis_dap_init(void)
{
    s_connected = false;
    swd_init();
}

/* ---- DAP_INFO ---- */
static int handle_info(const uint8_t *req, int req_len,
                       uint8_t *resp, int resp_max)
{
    (void)req_len;
    uint8_t id = req[1];
    int rlen = 2;  /* cmd + length byte */
    const char *str = NULL;
    uint8_t val = 0;

    switch (id) {
    case DAP_ID_VENDOR:          str = "ESP32"; break;
    case DAP_ID_PRODUCT:         str = "Wireless Serial Debugger"; break;
    case DAP_ID_SER_NUM:         str = "ESP32S3"; break;
    case DAP_ID_FW_VER:          str = "1.0"; break;
    case DAP_ID_DEVICE_VENDOR:   str = "Espressif"; break;
    case DAP_ID_DEVICE_NAME:     str = "ESP32-S3"; break;
    case DAP_ID_CAPABILITIES:
        val = 0x01;  /* bit0 = SWD supported */
        resp[rlen++] = val;
        goto done;
    case DAP_ID_SWD_VER:
        val = 0x01;  /* SWD v1 */
        resp[rlen++] = val;
        goto done;
    default:
        break;
    }

    if (str) {
        int slen = strlen(str);
        if (slen > resp_max - 3) slen = resp_max - 3;
        resp[rlen++] = (uint8_t)slen;
        memcpy(resp + rlen, str, slen);
        rlen += slen;
    } else {
        resp[rlen++] = 0;  /* unknown ID, return 0 length */
    }

done:
    resp[0] = DAP_INFO;
    resp[1] = (uint8_t)(rlen - 2);  /* data length */
    return rlen;
}

/* ---- DAP_CONNECT ---- */
static int handle_connect(const uint8_t *req, int req_len,
                          uint8_t *resp, int resp_max)
{
    (void)req; (void)req_len; (void)resp_max;
    /* Default to SWD mode: line reset -> JTAG-to-SWD -> line reset */
    swd_line_reset();
    swd_jtag_to_swd();
    swd_line_reset();
    s_connected = true;
    resp[0] = DAP_CONNECT;
    resp[1] = DAP_MODE_SWD;
    ESP_LOGI(TAG, "CONNECT (SWD)");
    return 2;
}

/* ---- DAP_DISCONNECT ---- */
static int handle_disconnect(const uint8_t *req, int req_len,
                             uint8_t *resp, int resp_max)
{
    (void)req; (void)req_len; (void)resp_max;
    s_connected = false;
    resp[0] = DAP_DISCONNECT;
    resp[1] = 1;  /* success */
    ESP_LOGI(TAG, "DISCONNECT");
    return 2;
}

/* ---- DAP_TRANSFER_CONFIGURE ---- */
static int handle_transfer_configure(const uint8_t *req, int req_len,
                                     uint8_t *resp, int resp_max)
{
    (void)req_len; (void)resp_max;
    /* req[1..5]: idle_cycles, wait_retry, match_retry, match_mask[0..3] */
    /* We don't use these in bit-bang mode */
    resp[0] = DAP_TRANSFER_CONFIGURE;
    resp[1] = 1;  /* success */
    return 2;
}

/* ---- DAP_SWJ_CLOCK ---- */
static int handle_swj_clock(const uint8_t *req, int req_len,
                            uint8_t *resp, int resp_max)
{
    (void)req_len; (void)resp_max;
    if (req_len >= 5) {
        s_clock_hz = req[1] | (req[2] << 8) | (req[3] << 16) | (req[4] << 24);
        ESP_LOGI(TAG, "SWJ_CLOCK %lu Hz", (unsigned long)s_clock_hz);
    }
    resp[0] = DAP_SWJ_CLOCK;
    resp[1] = 1;  /* success */
    return 2;
}

/* ---- DAP_SWD_CONFIGURE ---- */
static int handle_swd_configure(const uint8_t *req, int req_len,
                                uint8_t *resp, int resp_max)
{
    (void)req; (void)req_len; (void)resp_max;
    resp[0] = DAP_SWD_CONFIGURE;
    resp[1] = 1;  /* success */
    return 2;
}

/* ---- DAP_SWJ_SEQUENCE ---- */
static int handle_swj_sequence(const uint8_t *req, int req_len,
                               uint8_t *resp, int resp_max)
{
    (void)resp_max;
    /* req[1] = bit count, req[2..] = data bytes */
    if (req_len < 2) { resp[0] = DAP_SWJ_SEQUENCE; resp[1] = 0; return 2; }
    int bit_count = req[1];
    int byte_count = (bit_count + 7) / 8;
    if (req_len < 2 + byte_count) { resp[0] = DAP_SWJ_SEQUENCE; resp[1] = 0; return 2; }

    const uint8_t *data = req + 2;

    /* Check if this is a line reset (all 1s) or JTAG-to-SWD sequence */
    if (bit_count >= 50) {
        bool all_ones = true;
        for (int i = 0; i < byte_count; i++) {
            if (data[i] != 0xFF) { all_ones = false; break; }
        }
        if (all_ones) {
            swd_line_reset();
            resp[0] = DAP_SWJ_SEQUENCE;
            resp[1] = 1;
            return 2;
        }
    }

    /* Check for JTAG-to-SWD magic: 0xFF 0x92 0xF3 */
    if (bit_count == 16 && byte_count >= 2 && data[0] == 0x9E && data[1] == 0xE7) {
        /* This is the JTAG-to-SWD sequence in reverse bit order */
        swd_jtag_to_swd();
        resp[0] = DAP_SWJ_SEQUENCE;
        resp[1] = 1;
        return 2;
    }

    /* Check for JTAG-to-SWD in standard format */
    if (bit_count >= 16) {
        /* Look for the 0x79 0xE7 pattern (JTAG-to-SWD select sequence) */
        for (int i = 0; i < byte_count - 1; i++) {
            if (data[i] == 0x79 && data[i+1] == 0xE7) {
                swd_jtag_to_swd();
                resp[0] = DAP_SWJ_SEQUENCE;
                resp[1] = 1;
                return 2;
            }
        }
    }

    /* Generic sequence - perform line reset as fallback */
    if (bit_count >= 50) {
        swd_line_reset();
    }

    resp[0] = DAP_SWJ_SEQUENCE;
    resp[1] = 1;
    return 2;
}

/* ---- DAP_SWJ_PINS ---- */
static int handle_swj_pins(const uint8_t *req, int req_len,
                           uint8_t *resp, int resp_max)
{
    (void)resp_max;
    /* req[1] = pin output, req[2] = pin select, req[3..6] = wait timeout */
    if (req_len < 3) { resp[0] = DAP_SWJ_PINS; resp[1] = 0; return 2; }

    uint8_t output = req[1];
    uint8_t select = req[2];

    /* Handle NRST pin (bit 7) */
    if (select & 0x80) {
        swd_reset_target(!(output & 0x80));  /* active low */
    }

    resp[0] = DAP_SWJ_PINS;
    resp[1] = output;  /* return current pin state */
    return 2;
}

/* ---- DAP_RESET_TARGET ---- */
static int handle_reset_target(const uint8_t *req, int req_len,
                               uint8_t *resp, int resp_max)
{
    (void)req; (void)req_len; (void)resp_max;
    swd_reset_target(true);
    /* small delay */ ;
    swd_reset_target(false);
    resp[0] = DAP_RESET_TARGET;
    resp[1] = 1;  /* success */
    return 2;
}

/* ---- DAP_TRANSFER ---- */
static int handle_transfer(const uint8_t *req, int req_len,
                           uint8_t *resp, int resp_max)
{
    /* req[0]=DAP_TRANSFER, req[1]=DAP index(ignored), req[2]=count, req[3..]=transfers */
    resp[0] = DAP_TRANSFER;

    if (req_len < 3) {
        resp[1] = 0;  /* count */
        resp[2] = DAP_TRANSFER_ERROR;
        return 3;
    }

    uint8_t count = req[2];
    int pos = 3;
    int rpos = 3;  /* response write position */
    uint8_t resp_count = 0;

    for (int i = 0; i < count && pos < req_len && rpos < resp_max - 4; i++) {
        uint8_t request = req[pos++];
        bool is_read = (request & DAP_TRANSFER_RnW) != 0;
        bool is_ap   = (request & DAP_TRANSFER_APnDP) != 0;
        uint8_t addr  = (request & 0x0C);  /* A[2:3] -> reg addr bits [2:3] */

        esp_err_t err = ESP_OK;
        uint32_t data = 0;

        if (is_read) {
            if (is_ap) {
                err = swd_read_ap(addr, &data);
            } else {
                err = swd_read_dp(addr, &data);
            }
            if (err == ESP_OK) {
                resp[rpos++] = DAP_TRANSFER_OK;
                resp[rpos++] = (uint8_t)(data);
                resp[rpos++] = (uint8_t)(data >> 8);
                resp[rpos++] = (uint8_t)(data >> 16);
                resp[rpos++] = (uint8_t)(data >> 24);
            } else {
                resp[rpos++] = DAP_TRANSFER_FAULT;
            }
        } else {
            if (pos + 4 > req_len) {
                resp[rpos++] = DAP_TRANSFER_ERROR;
                break;
            }
            data = req[pos] | (req[pos+1] << 8) | (req[pos+2] << 16) | (req[pos+3] << 24);
            pos += 4;
            if (is_ap) {
                err = swd_write_ap(addr, data);
            } else {
                err = swd_write_dp(addr, data);
            }
            if (err == ESP_OK) {
                resp[rpos++] = DAP_TRANSFER_OK;
            } else {
                resp[rpos++] = DAP_TRANSFER_FAULT;
            }
        }
        resp_count++;
    }

    resp[1] = resp_count;  /* number of transfers processed */
    resp[2] = 0;           /* no protocol error at end */

    /* Check if there was a FAULT in any transfer */
    bool any_fault = false;
    for (int i = 3; i < rpos; i += 5) {  /* read responses are5 bytes */
        if (i < rpos && resp[i] != DAP_TRANSFER_OK) {
            any_fault = true;
            break;
        }
    }
    if (any_fault) {
        /* On fault, send ABORT to clear sticky bits */
        swd_write_dp(0, 0x1E);  /* DP ABORT register */
    }

    return rpos;
}

/* ---- DAP_TRANSFER_BLOCK ---- */
static int handle_transfer_block(const uint8_t *req, int req_len,
                                 uint8_t *resp, int resp_max)
{
    resp[0] = DAP_TRANSFER_BLOCK;

    if (req_len < 5) {
        resp[1] = 0; resp[2] = 0;
        resp[3] = DAP_TRANSFER_ERROR;
        return 4;
    }

    uint8_t count_lo = req[2];
    uint8_t count_hi = req[3];
    int count = count_lo | (count_hi << 8);
    uint8_t request = req[4];
    bool is_read = (request & DAP_TRANSFER_RnW) != 0;
    bool is_ap   = (request & DAP_TRANSFER_APnDP) != 0;
    uint8_t addr  = (request & 0x0C);

    int rpos = 3;  /* after count bytes */
    int processed = 0;

    if (is_read) {
        for (int i = 0; i < count && rpos + 4 < resp_max; i++) {
            uint32_t data = 0;
            esp_err_t err;
            if (is_ap) err = swd_read_ap(addr, &data);
            else       err = swd_read_dp(addr, &data);

            if (err == ESP_OK) {
                resp[rpos++] = (uint8_t)data;
                resp[rpos++] = (uint8_t)(data >> 8);
                resp[rpos++] = (uint8_t)(data >> 16);
                resp[rpos++] = (uint8_t)(data >> 24);
                processed++;
            } else {
                resp[rpos++] = DAP_TRANSFER_FAULT;
                break;
            }
        }
    } else {
        int dpos = 5;
        for (int i = 0; i < count && dpos + 4 <= req_len; i++) {
            uint32_t data = req[dpos] | (req[dpos+1] << 8) |
                            (req[dpos+2] << 16) | (req[dpos+3] << 24);
            dpos += 4;
            esp_err_t err;
            if (is_ap) err = swd_write_ap(addr, data);
            else       err = swd_write_dp(addr, data);

            if (err == ESP_OK) {
                processed++;
            } else {
                resp[rpos++] = DAP_TRANSFER_FAULT;
                break;
            }
        }
    }

    resp[1] = (uint8_t)(processed & 0xFF);
    resp[2] = (uint8_t)(processed >> 8);

    /* Check for fault */
    bool fault = false;
    for (int i = 3; i < rpos; i++) {
        if (resp[i] == DAP_TRANSFER_FAULT) { fault = true; break; }
    }
    if (!fault && rpos == 3) {
        /* All transfers succeeded, mark OK at end */
        resp[rpos++] = DAP_TRANSFER_OK;
    }

    return rpos;
}

/* ---- DAP_WRITE_ABORT ---- */
static int handle_write_abort(const uint8_t *req, int req_len,
                              uint8_t *resp, int resp_max)
{
    (void)resp_max;
    resp[0] = DAP_WRITE_ABORT;
    if (req_len >= 5) {
        uint32_t data = req[1] | (req[2] << 8) | (req[3] << 16) | (req[4] << 24);
        swd_write_dp(0, data);  /* DP ABORT register at addr 0, write */
        resp[1] = 1;
    } else {
        resp[1] = 0;
    }
    return 2;
}

/* ---- DAP_DELAY ---- */
static int handle_delay(const uint8_t *req, int req_len,
                        uint8_t *resp, int resp_max)
{
    (void)resp_max;
    /* req[1..2] = delay in microseconds */
    resp[0] = DAP_DELAY;
    resp[1] = 1;
    return 2;
}

/* ---- Main dispatch ---- */
int cmsis_dap_process(const uint8_t *req, int req_len,
                      uint8_t *resp, int resp_max)
{
    if (req_len < 1) return 0;

    uint8_t cmd = req[0];

    switch (cmd) {
    case DAP_INFO:                return handle_info(req, req_len, resp, resp_max);
    case DAP_CONNECT:             return handle_connect(req, req_len, resp, resp_max);
    case DAP_DISCONNECT:          return handle_disconnect(req, req_len, resp, resp_max);
    case DAP_TRANSFER_CONFIGURE:  return handle_transfer_configure(req, req_len, resp, resp_max);
    case DAP_TRANSFER:            return handle_transfer(req, req_len, resp, resp_max);
    case DAP_TRANSFER_BLOCK:      return handle_transfer_block(req, req_len, resp, resp_max);
    case DAP_TRANSFER_ABORT:
        resp[0] = DAP_TRANSFER_ABORT;
        resp[1] = 1;
        return 2;
    case DAP_WRITE_ABORT:         return handle_write_abort(req, req_len, resp, resp_max);
    case DAP_DELAY:               return handle_delay(req, req_len, resp, resp_max);
    case DAP_RESET_TARGET:        return handle_reset_target(req, req_len, resp, resp_max);
    case DAP_SWJ_PINS:            return handle_swj_pins(req, req_len, resp, resp_max);
    case DAP_SWJ_CLOCK:           return handle_swj_clock(req, req_len, resp, resp_max);
    case DAP_SWJ_SEQUENCE:        return handle_swj_sequence(req, req_len, resp, resp_max);
    case DAP_SWD_CONFIGURE:       return handle_swd_configure(req, req_len, resp, resp_max);
    default:
        ESP_LOGW(TAG, "Unknown cmd 0x%02X", cmd);
        resp[0] = cmd;
        resp[1] = 0;  /* unsupported */
        return 2;
    }
}
