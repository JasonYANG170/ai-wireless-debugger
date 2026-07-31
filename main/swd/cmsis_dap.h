#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* CMSIS-DAP v2 command IDs */
#define DAP_INFO                0x00
#define DAP_HOST_STATUS         0x01
#define DAP_CONNECT             0x02
#define DAP_DISCONNECT          0x03
#define DAP_TRANSFER_CONFIGURE  0x04
#define DAP_TRANSFER            0x05
#define DAP_TRANSFER_BLOCK      0x06
#define DAP_TRANSFER_ABORT      0x07
#define DAP_WRITE_ABORT         0x08
#define DAP_DELAY               0x09
#define DAP_RESET_TARGET        0x0A
#define DAP_SWJ_PINS            0x10
#define DAP_SWJ_CLOCK           0x11
#define DAP_SWJ_SEQUENCE        0x12
#define DAP_SWD_CONFIGURE       0x13
#define DAP_JTAG_SEQUENCE       0x14
#define DAP_JTAG_CONFIGURE      0x15
#define DAP_JTAG_IDCODE          0x16

/* DAP_INFO info IDs */
#define DAP_ID_VENDOR           0x01
#define DAP_ID_PRODUCT          0x02
#define DAP_ID_SER_NUM          0x03
#define DAP_ID_FW_VER           0x04
#define DAP_ID_DEVICE_VENDOR    0x05
#define DAP_ID_DEVICE_NAME      0x06
#define DAP_ID_CAPABILITIES     0xF0
#define DAP_ID_SWD_VER          0xF1
#define DAP_ID_JTAG_VER         0xF2
#define DAP_ID_UART_RX_BUF_SZ   0xFB
#define DAP_ID_UART_TX_BUF_SZ   0xFC

/* DAP_TRANSFER request bits */
#define DAP_TRANSFER_RnW        (1 << 1)
#define DAP_TRANSFER_APnDP      (1 << 0)
#define DAP_TRANSFER_MATCH      (1 << 4)
#define DAP_TRANSFER_TIMESTAMP   (1 << 7)

/* DAP_TRANSFER status values */
#define DAP_TRANSFER_OK         1
#define DAP_TRANSFER_WAIT       2
#define DAP_TRANSFER_FAULT      3
#define DAP_TRANSFER_ERROR      4
#define DAP_TRANSFER_MISMATCH   5

/* DAP connect modes */
#define DAP_MODE_SWD            1
#define DAP_MODE_JTAG           2

/* Process one CMSIS-DAP command packet.
 * req/req_len  = input command
 * resp/resp_max = output response buffer
 * Returns response length. */
int cmsis_dap_process(const uint8_t *req, int req_len,
                      uint8_t *resp, int resp_max);

/* Initialize the CMSIS-DAP handler. */
void cmsis_dap_init(void);

#ifdef __cplusplus
}
#endif
