#include "ws_server.h"
#include "protocol.h"

#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "driver/uart.h"
#include "cJSON.h"

#include "serial_bridge.h"
#include "swd_bridge.h"
#include "wifi_manager.h"
#include "pinout.h"

static const char *TAG = "ws";

/* ============================================================
 *  Client tracking
 * ============================================================
 *  Connected WS client descriptors are kept here.  The mutex
 *  guards the array so broadcast helpers (running on any
 *  task) can safely walk it.  Per-client sends are done via
 *  httpd_queue_work() + httpd_ws_send_frame_async(), which is
 *  the documented way to push a frame from a non-httpd task.
 * ============================================================ */
typedef struct {
    int  fd;        /* socket fd, -1 = free slot */
    bool closing;   /* set when a send fails; cleaned next broadcast */
} ws_client_t;

static ws_client_t      s_clients[WS_MAX_CLIENTS];
static SemaphoreHandle_t s_clients_mu = NULL;
static httpd_handle_t   s_server     = NULL;

/* mbedtls-style base64 (ESP-IDF ships mbedtls) */
#include "mbedtls/base64.h"
#include "esp_timer.h"

/* ============================================================
 *  Forward declarations
 * ============================================================ */
static esp_err_t ws_handler  (httpd_req_t *req);
static esp_err_t root_handler (httpd_req_t *req);

/* ============================================================
 *  Client-array helpers
 * ============================================================ */
static void clients_lock(void)   { if (s_clients_mu) xSemaphoreTake(s_clients_mu, portMAX_DELAY); }
static void clients_unlock(void) { if (s_clients_mu) xSemaphoreGive(s_clients_mu); }

static void client_add(int fd)
{
    clients_lock();
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_clients[i].fd == -1) {
            s_clients[i].fd      = fd;
            s_clients[i].closing  = false;
            ESP_LOGI(TAG, "client connected fd=%d (slot %d)", fd, i);
            clients_unlock();
            return;
        }
    }
    ESP_LOGW(TAG, "client list full; rejecting fd=%d", fd);
    clients_unlock();
}

static void client_remove(int fd)
{
    clients_lock();
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_clients[i].fd == fd) {
            s_clients[i].fd      = -1;
            s_clients[i].closing  = false;
            ESP_LOGI(TAG, "client disconnected fd=%d (slot %d)", fd, i);
            break;
        }
    }
    clients_unlock();
}

size_t ws_server_client_count(void)
{
    size_t n = 0;
    clients_lock();
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_clients[i].fd != -1) n++;
    }
    clients_unlock();
    return n;
}

/* ============================================================
 *  Async frame sender
 * ============================================================
 *  Queued onto the httpd task via httpd_queue_work().  We
 *  snapshot the fds under the mutex, then send without holding
 *  it (httpd_ws_send_frame_async may block on the transport).
 * ============================================================ */
typedef struct {
    int    fds[WS_MAX_CLIENTS];
    int    n;
    size_t plen;
    char   payload[WS_MAX_PAYLOAD];  /* NUL-terminated JSON */
} ws_async_send_arg_t;

static void ws_async_send_task(void *arg)
{
    ws_async_send_arg_t *a = (ws_async_send_arg_t *)arg;

    for (int i = 0; i < a->n; i++) {
        httpd_ws_frame_t frame = {
            .type    = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t *)a->payload,
            .len     = a->plen,
            .final   = true,
        };
        esp_err_t err = httpd_ws_send_frame_async(s_server, a->fds[i], &frame);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "async send to fd=%d failed: %s",
                     a->fds[i], esp_err_to_name(err));
            client_remove(a->fds[i]);
        }
    }
    free(a);
}

/* Build JSON, snapshot fds, queue async send to all clients. */
static esp_err_t broadcast_json(const char *json, size_t jlen)
{
    int fds[WS_MAX_CLIENTS];
    int n = 0;

    clients_lock();
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_clients[i].fd != -1 && !s_clients[i].closing) {
            fds[n++] = s_clients[i].fd;
        }
    }
    clients_unlock();

    if (n == 0) {
        return ESP_ERR_NOT_FOUND;  /* nobody listening */
    }

    ws_async_send_arg_t *a = calloc(1, sizeof(*a));
    if (!a) {
        ESP_LOGE(TAG, "broadcast: OOM");
        return ESP_ERR_NO_MEM;
    }
    if (jlen >= sizeof(a->payload)) jlen = sizeof(a->payload) - 1;
    memcpy(a->payload, json, jlen);
    a->payload[jlen] = '\0';
    a->plen = jlen;
    memcpy(a->fds, fds, sizeof(int) * n);
    a->n = n;

    esp_err_t err = httpd_queue_work(s_server, ws_async_send_task, a);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_queue_work failed: %s", esp_err_to_name(err));
        free(a);
    }
    return err;
}

/* ============================================================
 *  Broadcast helpers
 * ============================================================ */

/* base64-encode raw bytes into a heap buffer (NUL-terminated). Caller frees. */
static char *b64_encode(const uint8_t *src, size_t len)
{
    size_t out_len = 0;
    if (mbedtls_base64_encode(NULL, 0, &out_len, src, len) != 0) {
        return NULL;
    }
    char *buf = malloc(out_len + 1);
    if (!buf) return NULL;
    size_t written = 0;
    if (mbedtls_base64_encode((unsigned char *)buf, out_len + 1, &written,
                              src, len) != 0) {
        free(buf);
        return NULL;
    }
    buf[written] = '\0';
    return buf;
}

/* base64-decode a string into a heap buffer. *out_len receives decoded len. */
static uint8_t *b64_decode(const char *src, size_t *out_len)
{
    size_t need = 0;
    if (mbedtls_base64_decode(NULL, 0, &need,
                              (const unsigned char *)src, strlen(src)) != 0) {
        return NULL;
    }
    uint8_t *buf = malloc(need ? need : 1);
    if (!buf) return NULL;
    size_t got = 0;
    if (mbedtls_base64_decode(buf, need, &got,
                              (const unsigned char *)src, strlen(src)) != 0) {
        free(buf);
        return NULL;
    }
    *out_len = got;
    return buf;
}

esp_err_t ws_server_broadcast_uart(const uint8_t *data, size_t len, bool is_tx)
{
    if (!data || len == 0) return ESP_ERR_INVALID_ARG;

    char *b64 = b64_encode(data, len);
    if (!b64) {
        ESP_LOGE(TAG, "uart b64 encode OOM");
        return ESP_ERR_NO_MEM;
    }

    /* {"type":"uart","data":"<b64>","dir":"rx"|"tx"} */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, WS_FIELD_TYPE, WS_TYPE_UART);
    cJSON_AddStringToObject(root, WS_FIELD_DATA, b64);
    cJSON_AddStringToObject(root, WS_FIELD_DIR, is_tx ? WS_DIR_TX : WS_DIR_RX);

    char *json = cJSON_PrintUnformatted(root);
    esp_err_t err = broadcast_json(json, strlen(json));
    if (err == ESP_ERR_NOT_FOUND) err = ESP_OK;  /* no clients is OK for broadcast */

    free(json);
    cJSON_Delete(root);
    free(b64);
    return err;
}

esp_err_t ws_server_broadcast_wifi(const char *state, const char *ip)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, WS_FIELD_TYPE, WS_TYPE_WIFI);
    cJSON_AddStringToObject(root, WS_FIELD_STATE,
                            state ? state : WS_WIFI_DISCONNECTED);
    cJSON_AddStringToObject(root, WS_FIELD_IP, ip ? ip : "");

    char *json = cJSON_PrintUnformatted(root);
    esp_err_t err = broadcast_json(json, strlen(json));
    if (err == ESP_ERR_NOT_FOUND) err = ESP_OK;

    free(json);
    cJSON_Delete(root);
    return err;
}

esp_err_t ws_server_broadcast_btn(int btn_id, const char *event)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, WS_FIELD_TYPE, WS_TYPE_BTN);
    cJSON_AddNumberToObject(root, WS_FIELD_ID, btn_id);
    cJSON_AddStringToObject(root, WS_FIELD_EVENT,
                            event ? event : WS_BTN_PRESS);

    char *json = cJSON_PrintUnformatted(root);
    esp_err_t err = broadcast_json(json, strlen(json));
    if (err == ESP_ERR_NOT_FOUND) err = ESP_OK;

    free(json);
    cJSON_Delete(root);
    return err;
}

/* ============================================================
 *  Inbound message handling
 * ============================================================ */

/* Map wifi_state_t enum -> protocol string. */
static const char *wifi_state_str(wifi_state_t ws)
{
    switch (ws) {
        case WIFI_STATE_CONNECTED_STA: return WS_WIFI_CONNECTED;
        case WIFI_STATE_AP_MODE:      return WS_WIFI_AP;
        case WIFI_STATE_CONNECTING:
        case WIFI_STATE_DISCONNECTED:
        default:                       return WS_WIFI_DISCONNECTED;
    }
}

/* Queued work: run swd_bridge_handle() on the httpd task (slow bit-bang
 * would otherwise block the WS receive handler). */
typedef struct {
    int  fd;
    char cmd[16];
    char addr[16];
    char data[16];
} swd_work_t;

static void swd_handle_task(void *arg)
{
    swd_work_t *w = (swd_work_t *)arg;
    char resp[256] = {0};

    esp_err_t err = swd_bridge_handle(w->cmd, w->addr, w->data, resp, sizeof(resp));

    if (err == ESP_OK && resp[0] != '\0') {
        /* swd_bridge_handle() returns a complete JSON string in resp. */
        httpd_ws_frame_t frame = {
            .type    = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t *)resp,
            .len     = strlen(resp),
            .final   = true,
        };
        httpd_ws_send_frame_async(s_server, w->fd, &frame);
    } else {
        /* build a minimal error swd_resp */
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, WS_FIELD_TYPE,   WS_TYPE_SWD_RESP);
        cJSON_AddStringToObject(root, WS_FIELD_STATUS,  WS_SWD_ERR);
        cJSON_AddStringToObject(root, WS_FIELD_DATA,    "0x00");
        char *json = cJSON_PrintUnformatted(root);
        httpd_ws_frame_t frame = {
            .type    = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t *)json,
            .len     = strlen(json),
            .final   = true,
        };
        httpd_ws_send_frame_async(s_server, w->fd, &frame);
        free(json);
        cJSON_Delete(root);
    }
    free(w);
}

/* Parse an inbound JSON text frame and act on it. */
static esp_err_t handle_ws_message(httpd_req_t *req, const char *buf, size_t len)
{
    int fd = httpd_req_to_sockfd(req);
    cJSON *root = cJSON_ParseWithLength(buf, len);
    if (!root) {
        ESP_LOGW(TAG, "invalid JSON from fd=%d", fd);
        return ESP_FAIL;
    }

    cJSON *jtype = cJSON_GetObjectItem(root, WS_FIELD_TYPE);
    if (!cJSON_IsString(jtype)) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    const char *type = jtype->valuestring;

    if (strcmp(type, WS_TYPE_UART) == 0) {
        /* base64 decode -> serial_bridge_write() */
        cJSON *jdata = cJSON_GetObjectItem(root, WS_FIELD_DATA);
        if (cJSON_IsString(jdata)) {
            size_t got = 0;
            uint8_t *raw = b64_decode(jdata->valuestring, &got);
            if (raw) {
                size_t written = serial_bridge_write(raw, got);
                if (written != got) {
                    ESP_LOGW(TAG, "serial write short: %u/%u",
                             (unsigned)written, (unsigned)got);
                }
                free(raw);
            }
        }
    } else if (strcmp(type, WS_TYPE_CFG) == 0) {
        cJSON *jbaud = cJSON_GetObjectItem(root, WS_FIELD_BAUD);
        if (cJSON_IsNumber(jbaud)) {
            uint32_t baud = (uint32_t)jbaud->valuedouble;
            esp_err_t e = serial_bridge_set_baud(baud);
            ESP_LOGI(TAG, "cfg baud=%u -> %s", (unsigned)baud, esp_err_to_name(e));
        }
    } else if (strcmp(type, WS_TYPE_SWD) == 0) {
        swd_work_t *w = calloc(1, sizeof(*w));
        if (!w) { cJSON_Delete(root); return ESP_ERR_NO_MEM; }
        w->fd = fd;
        cJSON *jcmd  = cJSON_GetObjectItem(root, WS_FIELD_CMD);
        cJSON *jaddr = cJSON_GetObjectItem(root, WS_FIELD_ADDR);
        cJSON *jdata = cJSON_GetObjectItem(root, WS_FIELD_DATA);
        snprintf(w->cmd,  sizeof(w->cmd),  "%s", cJSON_IsString(jcmd)  ? jcmd->valuestring  : "");
        snprintf(w->addr, sizeof(w->addr), "%s", cJSON_IsString(jaddr) ? jaddr->valuestring : "");
        snprintf(w->data, sizeof(w->data), "%s", cJSON_IsString(jdata) ? jdata->valuestring : "");
        httpd_queue_work(s_server, swd_handle_task, w);
    } else if (strcmp(type, WS_TYPE_STATUS) == 0) {
        /* build & send status_resp directly via httpd_ws_send_frame (we are in httpd ctx) */
        char ipbuf[16] = {0};
        wifi_manager_get_ip_str(ipbuf, sizeof(ipbuf));
        const char *wstate = wifi_state_str(wifi_manager_get_state());

        uint32_t baud = 0;
        uart_get_baudrate(UART1_PORT_NUM, &baud);

        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, WS_FIELD_TYPE, WS_TYPE_STATUS_RESP);
        cJSON_AddStringToObject(resp, WS_FIELD_WIFI, wstate);
        cJSON_AddStringToObject(resp, WS_FIELD_IP,   ipbuf);
        cJSON_AddNumberToObject(resp, WS_FIELD_BAUD,      baud);
        cJSON_AddNumberToObject(resp, WS_FIELD_RX_COUNT,  serial_bridge_get_rx_count());
        cJSON_AddNumberToObject(resp, WS_FIELD_TX_COUNT,  serial_bridge_get_tx_count());
        cJSON_AddNumberToObject(resp, WS_FIELD_UPTIME,
                                 (uint32_t)(esp_timer_get_time() / 1000000));

        char *json = cJSON_PrintUnformatted(resp);
        httpd_ws_frame_t frame = {
            .type    = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t *)json,
            .len     = strlen(json),
            .final   = true,
        };
        /* Use the req-based send so it goes out on the right socket immediately */
        httpd_ws_send_frame(req, &frame);
        free(json);
        cJSON_Delete(resp);
    } else {
        ESP_LOGW(TAG, "unknown type \"%s\"", type);
    }

    cJSON_Delete(root);
    return ESP_OK;
}

/* ============================================================
 *  URI handlers
 * ============================================================ */

/* GET /ws - WebSocket handler.
 * The ESP-IDF framework performs the WS upgrade handshake automatically
 * (is_websocket=true on the URI descriptor).  After the handshake, this
 * handler is called once per incoming WebSocket frame.  We read the
 * frame, process it, and return ESP_OK to keep the connection alive. */
static esp_err_t ws_handler(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);

    /* Track the fd on first frame (handshake just completed). */
    bool known = false;
    clients_lock();
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_clients[i].fd == fd) { known = true; break; }
    }
    clients_unlock();
    if (!known) {
        client_add(fd);
    }

    /* Receive one WS frame into a stack buffer. */
    uint8_t buf[WS_MAX_PAYLOAD + 1];
    httpd_ws_frame_t frame = {
        .type    = HTTPD_WS_TYPE_TEXT,
        .payload = buf,
        .len     = sizeof(buf) - 1,
    };
    esp_err_t err = httpd_ws_recv_frame(req, &frame, sizeof(buf) - 1);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ws recv fd=%d failed: %s", fd, esp_err_to_name(err));
        client_remove(fd);
        return err;
    }

    switch (frame.type) {
    case HTTPD_WS_TYPE_CLOSE:
        client_remove(fd);
        break;
    case HTTPD_WS_TYPE_TEXT:
        if (frame.len > 0) {
            buf[frame.len] = '\0';
            handle_ws_message(req, (char *)buf, frame.len);
        }
        break;
    default:
        break;  /* ping/pong/binary: framework handles or ignore */
    }

    return ESP_OK;  /* keep the WS open for the next frame */
}

/* ---- Minimal status HTML for GET / ---- */
static const char s_status_html[] =
"<!DOCTYPE html>"
"<html><head><meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>Wireless Serial Debugger</title>"
"<style>"
"body{font-family:system-ui,sans-serif;background:#111;color:#eee;margin:0;padding:20px}"
"h1{color:#4af;margin:0 0 10px}"
".box{background:#222;padding:12px 16px;border-radius:8px;margin-bottom:12px}"
".stat{display:flex;justify-content:space-between;padding:4px 0}"
".label{color:#888}"
"#log{white-space:pre-wrap;font-family:monospace;font-size:13px;max-height:340px;"
"overflow-y:scroll;word-break:break-all;line-height:1.4}"
"#sendbar{display:flex;gap:8px;margin-top:8px}"
"#sendbar input{flex:1;background:#333;border:1px solid #555;color:#eee;border-radius:4px;padding:6px 10px}"
"#sendbar button{background:#4af;color:#111;border:none;border-radius:4px;padding:6px 16px;cursor:pointer;font-weight:600}"
"#sendbar button:hover{background:#6cf}"
".sel{background:#333;color:#eee;border:1px solid #555;border-radius:4px;padding:6px}"
"</style></head><body>"
"<h1>Wireless Serial Debugger</h1>"
"<div class=\"box\" id=\"status\"><em>connecting...</em></div>"
"<div class=\"box\">"
"<div class=\"stat\"><span class=\"label\">RX bytes</span><span id=\"rx\">0</span></div>"
"<div class=\"stat\"><span class=\"label\">TX bytes</span><span id=\"tx\">0</span></div>"
"<div class=\"stat\"><span class=\"label\">Uptime</span><span id=\"up\">0s</span></div>"
"</div>"
"<div class=\"box\">"
"<b>Serial Log</b>"
"<div id=\"log\">(waiting for data...)</div>"
"<div id=\"sendbar\">"
"<input id=\"sendtext\" placeholder=\"Type to send...\" autofocus>"
"<select id=\"addnl\" class=\"sel\"><option value=\"none\">No NL</option><option value=\"nl\" selected>\\n</option><option value=\"cr\">\\r</option><option value=\"crlf\">\\r\\n</option></select>"
"<button onclick=\"sendData()\">Send</button>"
"</div>"
"</div>"
"<div class=\"box\">"
"<b>Config</b> &nbsp; Baud:"
"<select id=\"baudsel\" class=\"sel\" onchange=\"setBaud()\">"
"<option value=\"9600\">9600</option>"
"<option value=\"115200\" selected>115200</option>"
"<option value=\"460800\">460800</option>"
"<option value=\"921600\">921600</option>"
"</select>"
" &nbsp; <label><input type=\"checkbox\" id=\"hexmode\">Hex display</label>"
" &nbsp; <button onclick=\"clrLog()\" class=\"sel\">Clear</button>"
"</div>"
"<script>"
"const ws=new WebSocket('ws://'+location.host+'/ws');"
"var hexMode=false;"
"function b64d(s){var b=atob(s),r='';for(var i=0;i<b.length;i++)r+=String.fromCharCode(b.charCodeAt(i));return r}"
"function b64hex(s){var b=atob(s),r='';for(var i=0;i<b.length;i++)r+=('0'+b.charCodeAt(i).toString(16)).slice(-2)+' ';return r.trim()}"
"ws.onopen=function(){document.getElementById('status').innerHTML='<b style=\\\"color:#4f4\\\">WS connected</b>';ws.send(JSON.stringify({type:'status'}))};"
"ws.onmessage=function(ev){var m=JSON.parse(ev.data);"
" if(m.type==='status_resp'){"
"  document.getElementById('status').innerHTML='<b>WiFi:</b> '+m.wifi+' &nbsp; <b>IP:</b> '+m.ip+' &nbsp; <b>Baud:</b> '+m.baud;"
"  document.getElementById('rx').textContent=m.rx_count;"
"  document.getElementById('tx').textContent=m.tx_count;"
"  document.getElementById('up').textContent=m.uptime+'s';"
" } else if(m.type==='uart'){"
"  var log=document.getElementById('log');"
"  if(log.textContent.indexOf('(waiting')>=0)log.textContent='';"
"  var txt=hexMode?b64hex(m.data):b64d(m.data);"
"  var prefix=m.dir==='rx'?'>>':'<<';"
"  log.textContent+=prefix+' '+txt+'\\n';"
"  log.scrollTop=log.scrollHeight;"
" } else if(m.type==='wifi'){"
"  document.getElementById('status').innerHTML='<b>WiFi:</b> '+m.state+' &nbsp; <b>IP:</b> '+(m.ip||'');"
" } else if(m.type==='btn'){"
"  var log=document.getElementById('log');"
"  log.textContent+='[btn#'+m.id+' '+m.event+']\\n';"
"  log.scrollTop=log.scrollHeight;"
" }"
"};"
"ws.onclose=function(){document.getElementById('status').innerHTML='<b style=\\\"color:#f44\\\">WS disconnected</b>';};"
"document.getElementById('hexmode').onchange=function(e){hexMode=e.target.checked;};"
"function sendData(){"
" var t=document.getElementById('sendtext').value;"
" if(!t)return;"
" var nl=document.getElementById('addnl').value;"
" if(nl==='nl')t+='\\n';"
" else if(nl==='cr')t+='\\r';"
" else if(nl==='crlf')t+='\\r\\n';"
" var b64=btoa(t);"
" ws.send(JSON.stringify({type:'uart',data:b64}));"
" document.getElementById('sendtext').value='';"
"}"
"document.getElementById('sendtext').addEventListener('keydown',function(e){if(e.key==='Enter')sendData()});"
"function setBaud(){var b=parseInt(document.getElementById('baudsel').value);ws.send(JSON.stringify({type:'cfg',baud:b}));}"
"function clrLog(){document.getElementById('log').textContent='';}"
"setInterval(function(){if(ws.readyState===1)ws.send(JSON.stringify({type:'status'}))},2000);"
"</script></body></html>";

static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, s_status_html, -1);
}

/* ============================================================
 *  Server startup
 * ============================================================ */
esp_err_t ws_server_start(void)
{
    if (s_server) {
        ESP_LOGW(TAG, "server already running");
        return ESP_OK;
    }

    if (!s_clients_mu) {
        s_clients_mu = xSemaphoreCreateMutex();
        if (!s_clients_mu) return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        s_clients[i].fd = -1;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port       = WS_SERVER_PORT;
    cfg.max_uri_handlers  = 8;
    cfg.stack_size        = 12288;
    cfg.max_open_sockets  = WS_MAX_CLIENTS + 2;
    cfg.lru_purge_enable  = true;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    /* GET /  - status HTML page */
    httpd_uri_t root = {
        .uri           = WS_STATUS_PATH,
        .method        = HTTP_GET,
        .handler       = root_handler,
        .is_websocket  = false,
    };
    httpd_register_uri_handler(s_server, &root);

    /* GET /ws - WebSocket upgrade + frame handler */
    httpd_uri_t ws = {
        .uri           = WS_PATH,
        .method        = HTTP_GET,
        .handler       = ws_handler,
        .is_websocket  = true,
        .handle_ws_control_frames = false,
    };
    httpd_register_uri_handler(s_server, &ws);

    ESP_LOGI(TAG, "WebSocket server on :%d (path %s, %d client slots)",
             WS_SERVER_PORT, WS_PATH, WS_MAX_CLIENTS);
    return ESP_OK;
}
