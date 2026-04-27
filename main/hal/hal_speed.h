#ifndef HAL_SPEED_H
#define HAL_SPEED_H

#include <stdint.h>

void hal_speed_init(void);

/**
 * Returns current speed in km/h.
 * Uses PCNT hardware counting + software debounce.
 * Returns 0 if no pulses received within the stopped timeout.
 */
uint16_t hal_speed_get_kmh(void);

/**
 * Returns accumulated distance in meters since last reset.
 * Uses PCNT raw hardware count for accuracy.
 */
uint32_t hal_speed_get_distance_m(void);
void hal_speed_reset_distance(void);

#endif
