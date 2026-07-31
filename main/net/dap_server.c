/*
 * dap_server.c - CMSIS-DAP TCP server (port 5555).
 *
 * Accepts one client at a time (OpenOCD/pyOCD). Each TCP message is one
 * CMSIS-DAP command; the response is sent back immediately.
 *
 * CMSIS-DAP v1 framing: first byte = command ID, rest = payload.
 * We read one byte to get the command, then read the expected payload
 * based on the command, then send the response.
 */
#include "dap_server.h"
#include "cmsis_dap.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "esp_log.h"

static const char *TAG = "dap_srv";

#define DAP_PORT       5555
#define DAP_BUF_SIZE   1024
#define DAP_TIMEOUT_S  120

static volatile int s_client_count = 0;
static volatile bool s_running = false;

static void dap_client_task(void *arg)
{
    int sock = (int)(intptr_t)arg;
    s_client_count = 1;

    uint8_t *rxbuf = malloc(DAP_BUF_SIZE);
    uint8_t *txbuf = malloc(DAP_BUF_SIZE);
    if (!rxbuf || !txbuf) {
        ESP_LOGE(TAG, "alloc failed");
        close(sock);
        free(rxbuf); free(txbuf);
        s_client_count = 0;
        vTaskDelete(NULL);
        return;
    }

    /* Set receive timeout */
    struct timeval tv = { .tv_sec = DAP_TIMEOUT_S };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ESP_LOGI(TAG, "client connected (fd=%d)", sock);

    while (s_running) {
        /* Read command byte */
        uint8_t cmd;
        int n = recv(sock, &cmd, 1, 0);
        if (n <= 0) break;

        rxbuf[0] = cmd;

        /* Read remaining bytes based on command type.
         * self_read = true for variable-length commands that read their own
         * payload inside the case (skip the generic read below). */
        int payload_len = 0;
        bool self_read = false;
        switch (cmd) {
        case DAP_INFO:                payload_len = 1; break;  /* info_id */
        case DAP_CONNECT:             payload_len = 0; break;
        case DAP_DISCONNECT:          payload_len = 0; break;
        case DAP_TRANSFER_CONFIGURE:  payload_len = 5; break;
        case DAP_TRANSFER:            /* variable: index(1) + count(1) + transfers */
            n = recv(sock, rxbuf + 1, 2, 0);
            if (n <= 0) goto disconnect;
            payload_len = 2;
            /* Read transfer entries */
            {
                int count = rxbuf[2];
                for (int i = 0; i < count; i++) {
                    n = recv(sock, rxbuf + 3 + i * 5, 1, 0);
                    if (n <= 0) goto disconnect;
                    payload_len++;
                    if (!(rxbuf[3 + i * 5] & DAP_TRANSFER_RnW)) {
                        /* Write: 4 more data bytes */
                        n = recv(sock, rxbuf + 3 + i * 5 + 1, 4, 0);
                        if (n <= 0) goto disconnect;
                        payload_len += 4;
                    }
                }
            }
            self_read = true;
            break;
        case DAP_TRANSFER_BLOCK:      /* index(1) + count(2) + request(1) + data */
            n = recv(sock, rxbuf + 1, 4, 0);
            if (n <= 0) goto disconnect;
            payload_len = 4;
            {
                int count = rxbuf[2] | (rxbuf[3] << 8);
                bool is_read = (rxbuf[4] & DAP_TRANSFER_RnW) != 0;
                if (!is_read) {
                    int data_bytes = count * 4;
                    if (data_bytes > DAP_BUF_SIZE - 5) data_bytes = DAP_BUF_SIZE - 5;
                    int got = 0;
                    while (got < data_bytes) {
                        n = recv(sock, rxbuf + 5 + got, data_bytes - got, 0);
                        if (n <= 0) goto disconnect;
                        got += n;
                    }
                    payload_len += data_bytes;
                }
            }
            self_read = true;
            break;
        case DAP_WRITE_ABORT:         payload_len = 4; break;
        case DAP_DELAY:               payload_len = 2; break;
        case DAP_RESET_TARGET:        payload_len = 0; break;
        case DAP_SWJ_PINS:            payload_len = 6; break;  /* out + mask + 4 wait */
        case DAP_SWJ_CLOCK:           payload_len = 4; break;
        case DAP_SWJ_SEQUENCE:        /* variable: bit_count(1) + data */
            n = recv(sock, rxbuf + 1, 1, 0);
            if (n <= 0) goto disconnect;
            payload_len = 1;
            {
                int byte_count = (rxbuf[1] + 7) / 8;
                if (byte_count > DAP_BUF_SIZE - 2) byte_count = DAP_BUF_SIZE - 2;
                int got = 0;
                while (got < byte_count) {
                    n = recv(sock, rxbuf + 2 + got, byte_count - got, 0);
                    if (n <= 0) goto disconnect;
                    got += n;
                }
                payload_len += byte_count;
            }
            self_read = true;
            break;
        case DAP_SWD_CONFIGURE:       payload_len = 1; break;
        default:
            /* Unknown command: read a few more bytes then process */
            payload_len = 0;
            break;
        }

        /* Read any remaining payload (only for fixed-length commands) */
        if (!self_read && payload_len > 0) {
            int got = 0;
            while (got < payload_len) {
                n = recv(sock, rxbuf + 1 + got, payload_len - got, 0);
                if (n <= 0) goto disconnect;
                got += n;
            }
        }

        /* Process the command */
        int resp_len = cmsis_dap_process(rxbuf, 1 + payload_len,
                                         txbuf, DAP_BUF_SIZE);
        if (resp_len > 0) {
            int sent = 0;
            while (sent < resp_len) {
                n = send(sock, txbuf + sent, resp_len - sent, 0);
                if (n <= 0) goto disconnect;
                sent += n;
            }
        }
    }

disconnect:
    ESP_LOGI(TAG, "client disconnected (fd=%d)", sock);
    close(sock);
    free(rxbuf);
    free(txbuf);
    s_client_count = 0;
    vTaskDelete(NULL);
}

static void dap_server_task(void *arg)
{
    (void)arg;
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        ESP_LOGE(TAG, "socket create failed");
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DAP_PORT),
        .sin_addr.s_addr = INADDR_ANY,
    };
    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind failed");
        close(server_fd);
        vTaskDelete(NULL);
        return;
    }

    listen(server_fd, 1);
    ESP_LOGI(TAG, "CMSIS-DAP server listening on :%d", DAP_PORT);

    while (s_running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) continue;

        if (s_client_count > 0) {
            /* Only one client at a time */
            const char *msg = "BUSY\n";
            send(client_fd, msg, strlen(msg), 0);
            close(client_fd);
            continue;
        }

        /* Spawn client handler task */
        xTaskCreate(dap_client_task, "dap_client", 8192,
                    (void *)(intptr_t)client_fd, 6, NULL);
    }

    close(server_fd);
    vTaskDelete(NULL);
}

esp_err_t dap_server_start(void)
{
    if (s_running) return ESP_OK;
    s_running = true;
    cmsis_dap_init();
    xTaskCreate(dap_server_task, "dap_server", 4096, NULL, 5, NULL);
    return ESP_OK;
}

void dap_server_stop(void)
{
    s_running = false;
}

int dap_server_client_count(void) { return s_client_count; }
