/*
 * wifi_manager.c - WiFi STA+AP dual-mode manager for ESP32-S3 wireless serial debugger
 *
 * Flow:
 *   wifi_manager_init()
 *     -> read NVS credentials
 *     -> if found: start STA, wait up to 10s / 3 retries for IP
 *         -> on success: CONNECTED_STA
 *         -> on failure: fallback to AP
 *     -> if not found: start AP immediately
 *
 * Event group bits signal connection outcome to the init flow.
 */
#include "wifi_manager.h"

#include <string.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

static const char *TAG = "wifi_mgr";

/* Event group bits */
#define WIFI_CONNECTED_BIT  (1 << 0)
#define WIFI_FAIL_BIT        (1 << 1)

/* Internal state */
static wifi_state_t s_state = WIFI_STATE_DISCONNECTED;
static EventGroupHandle_t s_wifi_event_group = NULL;
static char s_ip_str[16] = {0};
static int s_retry_count = 0;
static bool s_wifi_started = false;
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;
static esp_event_handler_instance_t s_wifi_any_id;
static esp_event_handler_instance_t s_got_ip;

/* ---------- forward decls ---------- */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data);
static esp_err_t start_ap_mode_internal(void);
static esp_err_t connect_sta_internal(const char *ssid, const char *password);
static bool read_nvs_credentials(char *ssid, size_t ssid_len,
                                 char *pass, size_t pass_len);
static esp_err_t write_nvs_credentials(const char *ssid, const char *password);
static void build_ap_ssid(char *buf, size_t buf_len);

/* ---------- event handler ---------- */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "STA started, connecting...");
            s_state = WIFI_STATE_CONNECTING;
            esp_wifi_connect();
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            s_retry_count++;
            if (s_retry_count <= WIFI_STA_MAX_RETRY) {
                ESP_LOGW(TAG, "STA disconnected, retry %d/%d",
                         s_retry_count, WIFI_STA_MAX_RETRY);
                esp_wifi_connect();
            } else {
                ESP_LOGE(TAG, "STA failed after %d retries, falling back to AP",
                         WIFI_STA_MAX_RETRY);
                s_state = WIFI_STATE_DISCONNECTED;
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            }
            break;
        }

        case WIFI_EVENT_AP_START:
            ESP_LOGI(TAG, "AP started");
            s_state = WIFI_STATE_AP_MODE;
            strlcpy(s_ip_str, "192.168.4.1", sizeof(s_ip_str));
            break;

        default:
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "STA got IP: %s", s_ip_str);
        s_state = WIFI_STATE_CONNECTED_STA;
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* ---------- AP mode ---------- */
static void build_ap_ssid(char *buf, size_t buf_len)
{
    uint8_t mac[6] = {0};
    /* Use base MAC (ESP32 WiFi MAC) - available before WiFi start via esp_read_mac */
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(buf, buf_len, "%s%02X%02X",
             WIFI_AP_SSID_PREFIX, mac[4], mac[5]);
}

static esp_err_t start_ap_mode_internal(void)
{
    ESP_LOGI(TAG, "Starting AP mode");

    /* Create AP netif if not already created */
    if (s_ap_netif == NULL) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }

    /* Init WiFi driver if needed */
    if (!s_wifi_started) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        s_wifi_started = true;
    }

    /* Build AP SSID from MAC suffix */
    char ap_ssid[32];
    build_ap_ssid(ap_ssid, sizeof(ap_ssid));

    wifi_config_t ap_config = {
        .ap = {
            .channel = WIFI_AP_CHANNEL,
            .max_connection = WIFI_AP_MAX_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .required = false,
                .capable = false,
            },
        },
    };
    /* Use strlcpy for safety; password is a fixed string */
    strlcpy((char *)ap_config.ap.ssid, ap_ssid, sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen(ap_ssid);
    strlcpy((char *)ap_config.ap.password, WIFI_AP_PASSWORD,
            sizeof(ap_config.ap.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_state = WIFI_STATE_AP_MODE;
    strlcpy(s_ip_str, "192.168.4.1", sizeof(s_ip_str));

    ESP_LOGI(TAG, "AP ready: SSID=\"%s\", PW=\"%s\", IP=%s",
             ap_ssid, WIFI_AP_PASSWORD, s_ip_str);
    return ESP_OK;
}

/* ---------- STA connect ---------- */
static esp_err_t connect_sta_internal(const char *ssid, const char *password)
{
    ESP_LOGI(TAG, "Connecting to STA: %s", ssid);

    if (s_sta_netif == NULL) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
    }

    if (!s_wifi_started) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        s_wifi_started = true;
    }

    wifi_config_t sta_config = {0};
    strlcpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid));
    strlcpy((char *)sta_config.sta.password, password,
            sizeof(sta_config.sta.password));
    /* Enforce WPA2 minimum; allow open if password empty */
    sta_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));

    /* Reset state for a fresh attempt */
    s_retry_count = 0;
    s_state = WIFI_STATE_CONNECTING;
    s_ip_str[0] = '\0';
    xEventGroupClearBits(s_wifi_event_group,
                         WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    ESP_ERROR_CHECK(esp_wifi_start());

    /* Wait for connected or fail within timeout */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(WIFI_STA_TIMEOUT_MS));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to %s, IP: %s", ssid, s_ip_str);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "STA connection to %s timed out / failed", ssid);
    s_state = WIFI_STATE_DISCONNECTED;
    return ESP_FAIL;
}

/* ---------- NVS helpers ---------- */
static bool read_nvs_credentials(char *ssid, size_t ssid_len,
                                 char *pass, size_t pass_len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No stored WiFi credentials (NVS open failed: %s)",
                 esp_err_to_name(err));
        return false;
    }

    /* ssid is required; password may be empty for open networks */
    err = nvs_get_str(h, WIFI_NVS_KEY_SSID, ssid, &ssid_len);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No SSID in NVS");
        nvs_close(h);
        return false;
    }

    err = nvs_get_str(h, WIFI_NVS_KEY_PASS, pass, &pass_len);
    if (err != ESP_OK) {
        /* SSID present but no password - treat as open network */
        pass[0] = '\0';
    }

    nvs_close(h);
    ESP_LOGI(TAG, "Loaded credentials: SSID=\"%s\"", ssid);
    return true;
}

static esp_err_t write_nvs_credentials(const char *ssid, const char *password)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open for write failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(h, WIFI_NVS_KEY_SSID, ssid);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS set SSID failed: %s", esp_err_to_name(err));
        nvs_close(h);
        return err;
    }

    err = nvs_set_str(h, WIFI_NVS_KEY_PASS, password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS set password failed: %s", esp_err_to_name(err));
        nvs_close(h);
        return err;
    }

    err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS commit failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Credentials saved: SSID=\"%s\"", ssid);
    return ESP_OK;
}

/* ---------- Public API ---------- */
esp_err_t wifi_manager_init(void)
{
    ESP_LOGI(TAG, "Initialising WiFi manager");

    /* Create event group if not already present */
    if (s_wifi_event_group == NULL) {
        s_wifi_event_group = xEventGroupCreate();
    }

    /* netif init must be called before creating wifi netifs.
     * main.c already created the default event loop, so we only init netif. */
    ESP_ERROR_CHECK(esp_netif_init());

    /* Register event handlers (only once) */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &s_wifi_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &s_got_ip));

    /* Try to read stored credentials */
    char ssid[WIFI_SSID_MAX_LEN] = {0};
    char pass[WIFI_PASS_MAX_LEN] = {0};
    bool has_creds = read_nvs_credentials(ssid, sizeof(ssid), pass, sizeof(pass));

    /* Silence noisy WiFi deauth spam */
    esp_log_level_set("wifi", ESP_LOG_ERROR);
    esp_log_level_set("wifi_mgr", ESP_LOG_INFO);

    if (has_creds && strlen(ssid) > 0) {
        esp_err_t ret = connect_sta_internal(ssid, pass);
        if (ret == ESP_OK) {
            return ESP_OK;
        }
        /* STA failed - fall through to AP */
        ESP_LOGW(TAG, "STA failed, falling back to AP");
        /* Stop STA before starting AP */
        esp_wifi_stop();
        esp_wifi_set_mode(WIFI_MODE_NULL);
    }

    /* No credentials or STA failed -> AP mode */
    return start_ap_mode_internal();
}

wifi_state_t wifi_manager_get_state(void)
{
    return s_state;
}

esp_err_t wifi_manager_get_ip_str(char *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(buf, s_ip_str, len);
    return ESP_OK;
}

esp_err_t wifi_manager_set_credentials(const char *ssid, const char *password)
{
    if (ssid == NULL || password == NULL || strlen(ssid) == 0 ||
        strlen(ssid) >= WIFI_SSID_MAX_LEN ||
        strlen(password) >= WIFI_PASS_MAX_LEN) {
        ESP_LOGE(TAG, "Invalid credentials (ssid len=%d, pass len=%d)",
                 ssid ? (int)strlen(ssid) : -1,
                 password ? (int)strlen(password) : -1);
        return ESP_ERR_INVALID_ARG;
    }

    /* Persist to NVS */
    esp_err_t err = write_nvs_credentials(ssid, password);
    if (err != ESP_OK) {
        return err;
    }

    /* Tear down current mode and reconnect via STA */
    if (s_wifi_started) {
        esp_wifi_stop();
        esp_wifi_set_mode(WIFI_MODE_NULL);
        s_state = WIFI_STATE_DISCONNECTED;
        s_ip_str[0] = '\0';
    }

    return connect_sta_internal(ssid, password);
}

esp_err_t wifi_manager_clear_credentials(void)
{
    ESP_LOGI(TAG, "Clearing stored credentials");

    nvs_handle_t h;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        nvs_erase_key(h, WIFI_NVS_KEY_SSID);
        nvs_erase_key(h, WIFI_NVS_KEY_PASS);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "Credentials erased from NVS");
    }

    /* Stop WiFi and restart in AP mode */
    if (s_wifi_started) {
        esp_wifi_stop();
        esp_wifi_set_mode(WIFI_MODE_NULL);
        s_state = WIFI_STATE_DISCONNECTED;
        s_ip_str[0] = '\0';
    }

    return start_ap_mode_internal();
}

esp_err_t wifi_manager_start_ap(void)
{
    if (s_wifi_started) {
        esp_wifi_stop();
        esp_wifi_set_mode(WIFI_MODE_NULL);
        s_state = WIFI_STATE_DISCONNECTED;
        s_ip_str[0] = '\0';
    }
    return start_ap_mode_internal();
}

esp_err_t wifi_manager_connect_sta(void)
{
    char ssid[WIFI_SSID_MAX_LEN] = {0};
    char pass[WIFI_PASS_MAX_LEN] = {0};
    bool has_creds = read_nvs_credentials(ssid, sizeof(ssid), pass, sizeof(pass));
    if (!has_creds || strlen(ssid) == 0) {
        ESP_LOGW(TAG, "No stored credentials for STA connect");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_wifi_started) {
        esp_wifi_stop();
        esp_wifi_set_mode(WIFI_MODE_NULL);
        s_state = WIFI_STATE_DISCONNECTED;
        s_ip_str[0] = '\0';
    }

    return connect_sta_internal(ssid, pass);
}
