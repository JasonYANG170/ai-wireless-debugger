#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 *  WebSocket JSON protocol - Wireless Serial Debugger
 * ============================================================
 *
 *  Every message is a JSON object with a "type" field.  This
 *  header enumerates the type strings and the fields that
 *  accompany each message so client/server share one contract.
 *
 *  Direction notation:
 *    C->S  client  -> server   (sent over the WS by a browser)
 *    S->C  server  -> client    (pushed/broadcast by the ESP32)
 * ============================================================ */

/* ---- Maximum WebSocket JSON payload (tx & rx) ---- */
#define WS_MAX_PAYLOAD       4096

/* ---- WebSocket server port & path ---- */
#define WS_SERVER_PORT       80
#define WS_PATH              "/ws"
#define WS_STATUS_PATH       "/"

/* ---- Maximum concurrent WS clients ---- */
#define WS_MAX_CLIENTS       4

/* ============================================================
 *  Message type strings (value of the JSON "type" field)
 * ============================================================ */

/* S->C: serial data to/from DUT.
 *   {"type":"uart","data":"<base64>","dir":"rx"|"tx"}
 *  - data: base64-encoded bytes
 *  - dir:  "rx" = DUT->ESP32, "tx" = ESP32->DUT
 */
#define WS_TYPE_UART         "uart"

/* C->S: change UART configuration.
 *   {"type":"cfg","baud":115200,"databits":8,"stopbits":1,"parity":"none"}
 *  - parity: "none" | "even" | "odd"
 */
#define WS_TYPE_CFG          "cfg"

/* S->C: WiFi status change.
 *   {"type":"wifi","state":"connected"|"ap"|"disconnected","ip":"..."}
 */
#define WS_TYPE_WIFI         "wifi"

/* C->S: SWD command to the target.
 *   {"type":"swd","cmd":"reset"|"read_id"|"read_dp"|"write_dp"|"read_ap"|"write_ap",
 *    "addr":"0x..","data":"0x.."}
 *  - addr: hex string (without 0x), used for dp/ap reads & writes
 *  - data: hex string (without 0x), used for writes only
 */
#define WS_TYPE_SWD          "swd"

/* S->C: SWD command response.
 *   {"type":"swd_resp","status":"ok"|"error","data":"0x.."}
 */
#define WS_TYPE_SWD_RESP     "swd_resp"

/* S->C: physical button event.
 *   {"type":"btn","id":1,"event":"press"|"long_press"|"release"}
 */
#define WS_TYPE_BTN          "btn"

/* C->S: poll device status.
 *   {"type":"status"}
 */
#define WS_TYPE_STATUS      "status"

/* S->C: status response.
 *   {"type":"status_resp","wifi":"connected","ip":"...",
 *    "baud":115200,"rx_count":12345,"tx_count":6789,"uptime":3600}
 */
#define WS_TYPE_STATUS_RESP "status_resp"

/* ============================================================
 *  Field-name constants (used by cJSON parsing/building)
 * ============================================================ */
#define WS_FIELD_TYPE        "type"
#define WS_FIELD_DATA        "data"
#define WS_FIELD_DIR         "dir"
#define WS_FIELD_BAUD        "baud"
#define WS_FIELD_DATABITS    "databits"
#define WS_FIELD_STOPBITS    "stopbits"
#define WS_FIELD_PARITY      "parity"
#define WS_FIELD_STATE       "state"
#define WS_FIELD_IP          "ip"
#define WS_FIELD_CMD         "cmd"
#define WS_FIELD_ADDR        "addr"
#define WS_FIELD_STATUS      "status"
#define WS_FIELD_ID          "id"
#define WS_FIELD_EVENT       "event"
#define WS_FIELD_WIFI        "wifi"
#define WS_FIELD_RX_COUNT    "rx_count"
#define WS_FIELD_TX_COUNT    "tx_count"
#define WS_FIELD_UPTIME      "uptime"

/* ---- Convenience enum values for enums passed as strings ---- */

/* uart dir */
#define WS_DIR_RX            "rx"
#define WS_DIR_TX            "tx"

/* wifi state */
#define WS_WIFI_CONNECTED    "connected"
#define WS_WIFI_AP            "ap"
#define WS_WIFI_DISCONNECTED  "disconnected"

/* swd command */
#define WS_SWD_CMD_RESET     "reset"
#define WS_SWD_CMD_READ_ID   "read_id"
#define WS_SWD_CMD_READ_DP   "read_dp"
#define WS_SWD_CMD_WRITE_DP  "write_dp"
#define WS_SWD_CMD_READ_AP   "read_ap"
#define WS_SWD_CMD_WRITE_AP  "write_ap"

/* swd_resp status */
#define WS_SWD_OK            "ok"
#define WS_SWD_ERR           "error"

/* btn event */
#define WS_BTN_PRESS        "press"
#define WS_BTN_LONG_PRESS   "long_press"
#define WS_BTN_RELEASE      "release"

#ifdef __cplusplus
}
#endif
