/*
 * clk_mon.c - Clock frequency measurement via the PCNT pulse counter.
 *
 * Counts rising edges on `gpio` over a fixed gate window; the new (v2) PCNT
 * driver accumulates automatically when watchpoints are set at the limits, so
 * pcnt_unit_get_count returns the total since start. A background task computes
 * the per-gate delta and reports it as a frequency.
 */
#include "clk_mon.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/pulse_cnt.h"
#include "esp_log.h"

static const char *TAG = "clk";

#define GATE_MS       100          /* measurement window */
#define PCNT_HIGH     10000        /* watchpoint / accumulation limit */
#define PCNT_LOW      (-1)

static pcnt_unit_handle_t s_unit = NULL;
static TaskHandle_t       s_task = NULL;
static volatile float     s_freq = 0.0f;

static void clk_task(void *arg)
{
    (void)arg;
    int last = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(GATE_MS));
        if (!s_unit) continue;

        int count = 0;
        pcnt_unit_get_count(s_unit, &count);   /* accumulated rising edges */
        int delta = (int)count - last;
        last = (int)count;
        /* keep the accumulated counter bounded (avoid int overflow over hours) */
        if ((int)count > 1000000) {
            pcnt_unit_clear_count(s_unit);
            last = 0;
        }
        s_freq = (delta > 0) ? ((float)delta * (1000.0f / GATE_MS)) : 0.0f;
    }
}

esp_err_t clk_mon_start(int gpio)
{
    if (s_unit) return ESP_ERR_INVALID_STATE;

    pcnt_unit_config_t ucfg = {
        .low_limit  = PCNT_LOW,
        .high_limit = PCNT_HIGH,
    };
    esp_err_t e = pcnt_new_unit(&ucfg, &s_unit);
    if (e != ESP_OK) { ESP_LOGE(TAG, "pcnt_new_unit: %s", esp_err_to_name(e)); return e; }

    /* glitch filter: ignore pulses < ~100 ns */
    pcnt_glitch_filter_config_t gf = { .max_glitch_ns = 100 };
    pcnt_unit_set_glitch_filter(s_unit, &gf);

    pcnt_chan_config_t ccfg = {
        .edge_gpio_num  = gpio,
        .level_gpio_num = -1,
    };
    pcnt_channel_handle_t chan = NULL;
    e = pcnt_new_channel(s_unit, &ccfg, &chan);
    if (e != ESP_OK) { ESP_LOGE(TAG, "pcnt_new_channel: %s", esp_err_to_name(e)); return e; }

    /* count on rising edge; level input unused */
    pcnt_channel_set_edge_action(chan, PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                 PCNT_CHANNEL_EDGE_ACTION_HOLD);
    pcnt_channel_set_level_action(chan, PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                  PCNT_CHANNEL_LEVEL_ACTION_KEEP);

    /* watchpoints at the limits enable the driver's overflow accumulation */
    pcnt_unit_add_watch_point(s_unit, PCNT_HIGH);
    pcnt_unit_add_watch_point(s_unit, PCNT_LOW);

    pcnt_unit_enable(s_unit);
    pcnt_unit_clear_count(s_unit);
    pcnt_unit_start(s_unit);

    xTaskCreate(clk_task, "clk_mon", 2048, NULL, 5, &s_task);
    ESP_LOGI(TAG, "clock monitor on GPIO%d", gpio);
    return ESP_OK;
}

float clk_mon_get_freq(void)
{
    return s_freq;
}
