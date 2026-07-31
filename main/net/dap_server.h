#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start the CMSIS-DAP TCP server on port 5555.
 * One client at a time; OpenOCD connects here. */
esp_err_t dap_server_start(void);

/* Stop the server. */
void dap_server_stop(void);

/* Number of currently connected clients (0 or 1). */
int dap_server_client_count(void);

#ifdef __cplusplus
}
#endif
