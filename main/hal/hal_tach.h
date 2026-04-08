#ifndef HAL_TACH_H
#define HAL_TACH_H

#include <stdint.h>

void hal_tach_init(void);

/**
 * Returns current engine RPM based on the period between
 * the last two pulses from the tachometer signal.
 * Returns 0 if no pulses received within the timeout window.
 */
uint16_t hal_tach_get_rpm(void);

#endif
