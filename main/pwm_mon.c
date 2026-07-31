/*
 * pwm_mon.c - PWM frequency + duty measurement via GPIO edge timestamps.
 *
 * A both-edge ISR on the PWM pin records the time of each edge. From the
 * rising-to-rising interval we get the period (=> frequency), and from the
 * rising-to-falling interval we get the high time (=> duty cycle).
 */
#include "pwm_mon.h"
#include <stdint.h>
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "pwm";

static int s_gpio = -1;

/* Updated in ISR; read with single-copy accesses (int64_t reads are atomic
 * enough on Xtensa for this informational use). */
static volatile int64_t s_last_rise_us = 0;
static volatile int64_t s_last_edge_us = 0;
static volatile int64_t s_period_us    = 0;   /* rising -> rising */
static volatile int64_t s_high_us      = 0;   /* rising -> falling */
static volatile uint32_t s_isr_count   = 0;   /* diagnostic: total ISR invocations */

static void IRAM_ATTR pwm_isr_handler(void *arg)
{
    (void)arg;
    int64_t t = esp_timer_get_time();
    s_last_edge_us = t;
    s_isr_count++;
    if (gpio_get_level(s_gpio)) {
        /* rising edge: a period just completed */
        if (s_last_rise_us) {
            s_period_us = t - s_last_rise_us;
        }
        s_last_rise_us = t;
    } else {
        /* falling edge: high time of the current period */
        if (s_last_rise_us) {
            s_high_us = t - s_last_rise_us;
        }
    }
}

esp_err_t pwm_mon_start(int gpio)
{
    s_gpio = gpio;
    s_last_rise_us = s_last_edge_us = 0;
    s_period_us = s_high_us = 0;

    /* Explicitly reset the GPIO before configuration — previous boot may have
     * left it in SWD/UART/output mode, which prevents edge interrupts. */
    gpio_reset_pin(gpio);

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << gpio,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,   /* idle low: unconnected reads 0 */
        .intr_type    = GPIO_INTR_ANYEDGE,
    };
    esp_err_t e = gpio_config(&cfg);
    if (e != ESP_OK) return e;

    /* buttons.c already installs the ISR service; tolerate "already installed". */
    e = gpio_install_isr_service(0);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;

    e = gpio_isr_handler_add(gpio, pwm_isr_handler, NULL);
    if (e != ESP_OK) return e;

    ESP_LOGI(TAG, "PWM monitor on GPIO%d", gpio);
    return ESP_OK;
}

void pwm_mon_stop(void)
{
    if (s_gpio >= 0) {
        gpio_isr_handler_remove(s_gpio);
        gpio_set_intr_type(s_gpio, GPIO_INTR_DISABLE);
        gpio_reset_pin(s_gpio);
    }
    s_gpio = -1;
    s_last_rise_us = s_last_edge_us = 0;
    s_period_us = s_high_us = 0;
    s_isr_count = 0;
}

void pwm_mon_get(float *freq, float *duty)
{
    int64_t period = s_period_us;
    int64_t high   = s_high_us;
    int64_t last   = s_last_edge_us;
    int64_t now    = esp_timer_get_time();

    if (freq) *freq = 0.0f;
    if (duty) *duty = 0.0f;
    if (period <= 0 || (now - last) > 1000000) {
        return;   /* no signal yet, or stale (>1 s since last edge) */
    }
    if (freq) *freq = 1000000.0f / (float)period;
    if (duty) *duty = (float)high * 100.0f / (float)period;
}

uint32_t pwm_mon_isr_count(void) { return s_isr_count; }

/* ========== PWM Output (LEDC) ========== */
#include "driver/ledc.h"

static bool     s_out_running = false;
static int      s_out_gpio = -1;
static float    s_out_freq = 0;
static float    s_out_duty = 0;

esp_err_t pwm_out_start(int gpio, float freq, float duty)
{
    if (s_out_running) pwm_out_stop();
    if (gpio < 0 || freq <= 0) return ESP_ERR_INVALID_ARG;

    /* Use LEDC timer 0, channel 0, low-speed mode, 13-bit resolution */
    ledc_timer_config_t timer_cfg = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_13_BIT,
        .timer_num       = LEDC_TIMER_0,
        .freq_hz         = (uint32_t)freq,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    esp_err_t e = ledc_timer_config(&timer_cfg);
    if (e != ESP_OK) return e;

    ledc_channel_config_t ch_cfg = {
        .gpio_num   = gpio,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .timer_sel  = LEDC_TIMER_0,
        .duty       = (uint32_t)(duty * 8192.0f / 100.0f),
        .hpoint     = 0,
    };
    e = ledc_channel_config(&ch_cfg);
    if (e != ESP_OK) return e;

    s_out_gpio = gpio;
    s_out_freq = freq;
    s_out_duty = duty;
    s_out_running = true;
    ESP_LOGI(TAG, "PWM output on GPIO%d: %.1f Hz, %.1f%%", gpio, (double)freq, (double)duty);
    return ESP_OK;
}

esp_err_t pwm_out_set(float freq, float duty)
{
    if (!s_out_running) return ESP_ERR_INVALID_STATE;
    esp_err_t e = ESP_OK;
    if (freq != s_out_freq) {
        e = ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0, (uint32_t)freq);
        if (e != ESP_OK) return e;
        s_out_freq = freq;
    }
    uint32_t duty_val = (uint32_t)(duty * 8192.0f / 100.0f);
    e = ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty_val);
    if (e == ESP_OK) e = ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    if (e == ESP_OK) s_out_duty = duty;
    return e;
}

void pwm_out_stop(void)
{
    if (!s_out_running) return;
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    gpio_reset_pin(s_out_gpio);
    s_out_running = false;
    s_out_gpio = -1;
    ESP_LOGI(TAG, "PWM output stopped");
}

bool  pwm_out_running(void) { return s_out_running; }
void  pwm_out_get(float *freq, float *duty)
{
    if (freq) *freq = s_out_freq;
    if (duty) *duty = s_out_duty;
}
