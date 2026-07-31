#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start the raw TCP serial bridge server on port 3333.
 * Connected clients receive raw UART bytes from the DUT,
 * and anything sent to the TCP socket is forwarded to the DUT via UART1. */
esp_err_t tcp_server_start(void);

/* Broadcast raw bytes to all connected TCP clients.
 * Called from the serial RX callback when data arrives from DUT. */
void tcp_server_broadcast(const uint8_t *data, size_t len);

/* Returns the number of connected TCP clients. */
int tcp_server_client_count(void);

#ifdef __cplusplus
}
#endif
