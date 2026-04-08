#ifndef HAL_SPEED_H
#define HAL_SPEED_H

#include <stdint.h>

void hal_speed_init(void);

/**
 * Returns current speed in km/h based on the period between
 * the last two pulses from the vehicle speed sensor.
 * Returns 0 if no pulses received within the timeout window.
 */
uint16_t hal_speed_get_kmh(void);

/**
 * Returns accumulated distance in meters since last reset.
 * Call hal_speed_reset_distance() after persisting to odometer.
 */
uint32_t hal_speed_get_distance_m(void);
void hal_speed_reset_distance(void);

#endif
