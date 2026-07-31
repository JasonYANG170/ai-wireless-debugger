#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start frequency measurement on `gpio` using the PCNT pulse counter.
 * Good for clocks up to several MHz. */
esp_err_t clk_mon_start(int gpio);

/* Latest frequency in Hz (0 if no edges seen recently). */
float clk_mon_get_freq(void);

#ifdef __cplusplus
}
#endif
