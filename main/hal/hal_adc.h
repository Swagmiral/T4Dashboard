#ifndef HAL_ADC_H
#define HAL_ADC_H

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"

void hal_adc_init(i2c_master_bus_handle_t bus);

/** Start a single-shot conversion on the given AIN channel (0-3). */
bool hal_adc_start_conversion(uint8_t channel);

/**
 * Read result of previous conversion as raw voltage at the ADC pin.
 * Returns false if the read failed or device is not initialized.
 */
bool hal_adc_read_raw(float *v_adc_out);

bool hal_adc_ok(void);
bool hal_adc_reprobe(void);

#endif
