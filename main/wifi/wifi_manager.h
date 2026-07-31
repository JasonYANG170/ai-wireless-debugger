/*
 * wifi_manager.h - WiFi STA+AP dual-mode manager for ESP32-S3 wireless serial debugger
 *
 * Behaviour:
 *   - On init, read stored credentials from NVS and try STA mode.
 *   - If no credentials exist, or STA fails after 3 retries (10s total timeout),
 *     fall back to AP mode so the device remains reachable for configuration.
 *   - Credentials can be set/cleared at runtime; clearing restarts in AP mode.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* NVS storage */
#define WIFI_NVS_NAMESPACE   "wifi_cred"
#define WIFI_NVS_KEY_SSID    "ssid"
#define WIFI_NVS_KEY_PASS    "pass"

/* AP configuration */
#define WIFI_AP_SSID_PREFIX  "SerialDbg-"
#define WIFI_AP_PASSWORD      "12345678"
#define WIFI_AP_CHANNEL       1
#define WIFI_AP_MAX_CONN     2

/* STA retry/timeout */
#define WIFI_STA_MAX_RETRY   3
#define WIFI_STA_TIMEOUT_MS  10000

/* SSID/password buffer sizes (802.11 limits + NUL) */
#define WIFI_SSID_MAX_LEN    33
#define WIFI_PASS_MAX_LEN    65

typedef enum {
    WIFI_STATE_DISCONNECTED = 0,
    WIFI_STATE_CONNECTING,
    WIFI_STATE_CONNECTED_STA,
    WIFI_STATE_AP_MODE,
} wifi_state_t;

/*
 * Initialise the WiFi subsystem.
 *
 * Assumes the default event loop and NVS flash are already initialised by the
 * caller (main.c). Reads stored credentials; if present, attempts STA and
 * falls back to AP on failure. If no credentials, starts AP directly.
 *
 * Returns ESP_OK on success (always succeeds to AP mode at worst).
 */
esp_err_t wifi_manager_init(void);

/*
 * Current connection state.
 */
wifi_state_t wifi_manager_get_state(void);

/*
 * Write current IP address as a NUL-terminated dotted-quad string into buf.
 * In STA mode this is the DHCP-assigned address; in AP mode "192.168.4.1".
 * If no IP is available, writes an empty string.
 *
 * Returns ESP_OK on success.
 */
esp_err_t wifi_manager_get_ip_str(char *buf, size_t len);

/*
 * Persist new STA credentials to NVS and trigger a reconnect in STA mode.
 * ssid: NUL-terminated, max 32 chars. password: NUL-terminated, max 64 chars.
 *
 * Returns ESP_OK on success.
 */
esp_err_t wifi_manager_set_credentials(const char *ssid, const char *password);

/*
 * Erase stored credentials and restart in AP mode.
 */
esp_err_t wifi_manager_clear_credentials(void);

/*
 * Force AP mode immediately.
 */
esp_err_t wifi_manager_start_ap(void);

/*
 * Force a STA connection attempt using currently stored credentials.
 * If no credentials are stored, returns ESP_ERR_INVALID_STATE.
 */
esp_err_t wifi_manager_connect_sta(void);

#ifdef __cplusplus
}
#endif
