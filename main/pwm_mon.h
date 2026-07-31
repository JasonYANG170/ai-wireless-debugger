#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start PWM measurement on `gpio` using both-edge interrupts + esp_timer
 * timestamps. Good for Hz .. ~50 kHz PWM. */
esp_err_t pwm_mon_start(int gpio);

/* Stop PWM measurement (releases the interrupt). */
void pwm_mon_stop(void);

/* Latest reading: freq in Hz, duty in percent (0..100).
 * freq==0 means no signal (or stale > 1 s). */
void pwm_mon_get(float *freq, float *duty);

/* Total edge ISR invocations (diagnostic). */
uint32_t pwm_mon_isr_count(void);

/* ---- PWM Output (LEDC) ---- */

/* Start PWM output on `gpio` with given frequency (Hz) and duty (0..100%). */
esp_err_t pwm_out_start(int gpio, float freq, float duty);

/* Update PWM output frequency and duty. */
esp_err_t pwm_out_set(float freq, float duty);

/* Stop PWM output. */
void pwm_out_stop(void);

/* Is PWM output currently running? */
bool pwm_out_running(void);

/* Get current output freq/duty. */
void pwm_out_get(float *freq, float *duty);

#ifdef __cplusplus
}
#endif
