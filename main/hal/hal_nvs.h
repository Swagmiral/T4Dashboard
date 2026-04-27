#ifndef HAL_NVS_H
#define HAL_NVS_H

#include <stdint.h>
#include <stdbool.h>

void hal_nvs_init(void);

// Dashboard config (JSON in NVS + one-time migration from legacy CRC blob)
bool hal_nvs_load_config(void);
bool hal_nvs_save_config(void);

// Odometer (wear-leveled, CRC-protected)
uint32_t hal_nvs_get_odometer(void);
void hal_nvs_save_odometer(uint32_t km);
/** Add travelled meters to odometer (RAM + sub-km remainder). Persists when full km changes. */
uint32_t hal_nvs_add_distance_m(uint32_t delta_m);

/** Last displayed fuel 0–100% (separate from config JSON). */
bool hal_nvs_load_last_fuel_pct(uint8_t *out_pct);
void hal_nvs_save_last_fuel_pct(uint8_t pct);
void hal_nvs_erase_last_fuel_pct(void);

#endif
