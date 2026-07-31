/*
 * tcp_server.c - Raw TCP serial bridge server (port 3333)
 * Simple, reliable: connect with netcat/PuTTY, raw bytes in/out, no protocol overhead.
 */
#include "tcp_server.h"
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "serial_bridge.h"
#include "wifi_manager.h"
#include "pinout.h"

static const char *TAG = "tcp";

#define TCP_PORT          3333
#define TCP_MAX_CLIENTS   4
#define TCP_BUF_SIZE      512
#define TCP_TASK_STACK    6144
#define TCP_TASK_PRIO     6

static int       s_clients[TCP_MAX_CLIENTS];
static SemaphoreHandle_t s_clients_mu = NULL;
static int       s_listen_sock = -1;
static bool      s_running = false;

static void clients_lock(void)   { xSemaphoreTake(s_clients_mu, portMAX_DELAY); }
static void clients_unlock(void) { xSemaphoreGive(s_clients_mu); }

int tcp_server_client_count(void)
{
    int n = 0;
    clients_lock();
    for (int i = 0; i < TCP_MAX_CLIENTS; i++)
        if (s_clients[i] >= 0) n++;
    clients_unlock();
    return n;
}

/* Called from serial RX task: broadcast raw bytes to all TCP clients */
void tcp_server_broadcast(const uint8_t *data, size_t len)
{
    clients_lock();
    for (int i = 0; i < TCP_MAX_CLIENTS; i++) {
        int fd = s_clients[i];
        if (fd >= 0) {
            /* non-blocking send; if it fails we'll clean up in the recv task */
            int sent = send(fd, data, len, MSG_DONTWAIT | MSG_NOSIGNAL);
            if (sent < 0) {
                ESP_LOGD(TAG, "send to fd=%d failed, closing", fd);
                close(fd);
                s_clients[i] = -1;
            }
        }
    }
    clients_unlock();
}

/* Per-client receive task: read from socket -> serial_bridge_write */
static void client_recv_task(void *arg)
{
    int fd = (int)(intptr_t)arg;
    uint8_t buf[TCP_BUF_SIZE];

    while (s_running) {
        int n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            ESP_LOGI(TAG, "client fd=%d disconnected", fd);
            break;
        }
        serial_bridge_write(buf, n);
    }

    clients_lock();
    for (int i = 0; i < TCP_MAX_CLIENTS; i++) {
        if (s_clients[i] == fd) {
            s_clients[i] = -1;
            break;
        }
    }
    clients_unlock();
    close(fd);
    vTaskDelete(NULL);
}

/* Accept loop task */
static void tcp_server_task(void *arg)
{
    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(TCP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    s_listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (s_listen_sock < 0) {
        ESP_LOGE(TAG, "socket() failed: errno=%d", errno);
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(s_listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(s_listen_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "bind() failed: errno=%d", errno);
        close(s_listen_sock);
        vTaskDelete(NULL);
        return;
    }

    if (listen(s_listen_sock, TCP_MAX_CLIENTS) < 0) {
        ESP_LOGE(TAG, "listen() failed: errno=%d", errno);
        close(s_listen_sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "TCP server listening on port %d", TCP_PORT);

    while (s_running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int fd = accept(s_listen_sock, (struct sockaddr *)&client_addr, &addr_len);
        if (fd < 0) {
            if (errno == EINTR) continue;
            ESP_LOGE(TAG, "accept() failed: errno=%d", errno);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* Find a free slot */
        int slot = -1;
        clients_lock();
        for (int i = 0; i < TCP_MAX_CLIENTS; i++) {
            if (s_clients[i] < 0) { slot = i; break; }
        }
        if (slot >= 0) {
            s_clients[slot] = fd;
            clients_unlock();
            uint32_t ip = client_addr.sin_addr.s_addr;
            ESP_LOGI(TAG, "client connected fd=%d from %d.%d.%d.%d (slot %d)",
                     fd, ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF, slot);

            /* Spawn a receive task for this client */
            char task_name[24];
            snprintf(task_name, sizeof(task_name), "tcp_rx_%d", fd);
            xTaskCreate(client_recv_task, task_name, TCP_TASK_STACK,
                        (void *)(intptr_t)fd, TCP_TASK_PRIO, NULL);
        } else {
            clients_unlock();
            ESP_LOGW(TAG, "client list full, rejecting fd=%d", fd);
            const char *msg = "Server busy\n";
            send(fd, msg, strlen(msg), 0);
            close(fd);
        }
    }

    close(s_listen_sock);
    s_listen_sock = -1;
    vTaskDelete(NULL);
}

esp_err_t tcp_server_start(void)
{
    if (s_running) {
        ESP_LOGW(TAG, "already running");
        return ESP_OK;
    }

    if (!s_clients_mu) {
        s_clients_mu = xSemaphoreCreateMutex();
        if (!s_clients_mu) return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < TCP_MAX_CLIENTS; i++)
        s_clients[i] = -1;

    s_running = true;
    if (xTaskCreate(tcp_server_task, "tcp_srv", TCP_TASK_STACK,
                    NULL, TCP_TASK_PRIO, NULL) != pdTRUE) {
        s_running = false;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
