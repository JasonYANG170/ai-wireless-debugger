#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 *  WebSocket server - Wireless Serial Debugger
 *  --------------------------------------------------------
 *  HTTP server on port 80 with two URI handlers:
 *    GET /ws   - WebSocket upgrade, bidirectional JSON
 *    GET /     - simple status HTML page
 *
 *  Broadcast helpers are async-safe: they queue work onto the
 *  httpd task via httpd_queue_work(), so they can be called
 *  from the serial / button / wifi tasks without holding the
 *  HTTP transport lock themselves.
 * ============================================================ */

/* Start the HTTP+WS server. Returns ESP_OK on success. */
esp_err_t ws_server_start(void);

/*
 * Broadcast a UART data frame to every connected WS client.
 *  - data/len: raw bytes from/to the DUT
 *  - is_tx:    true = ESP32->DUT (dir="tx"), false = DUT->ESP32 (dir="rx")
 * Bytes are base64-encoded into the JSON payload.
 * Safe to call from the serial RX task.
 */
esp_err_t ws_server_broadcast_uart(const uint8_t *data, size_t len, bool is_tx);

/*
 * Broadcast a WiFi status change to every connected WS client.
 *  state: WS_WIFI_CONNECTED | WS_WIFI_AP | WS_WIFI_DISCONNECTED
 *  ip:    dotted-quad string (may be NULL when disconnected)
 */
esp_err_t ws_server_broadcast_wifi(const char *state, const char *ip);

/*
 * Broadcast a physical button event.
 *  btn_id: 1..3
 *  event:  WS_BTN_PRESS | WS_BTN_LONG_PRESS | WS_BTN_RELEASE
 */
esp_err_t ws_server_broadcast_btn(int btn_id, const char *event);

/* Returns the number of currently connected WS clients (0..WS_MAX_CLIENTS). */
size_t ws_server_client_count(void);

#ifdef __cplusplus
}
#endif
