#ifndef HAL_NVS_H
#define HAL_NVS_H

#include <stdint.h>
#include <stdbool.h>

void hal_nvs_init(void);

// Dashboard config (stored as CRC-protected blob, survives flashing)
bool hal_nvs_load_config(void);
bool hal_nvs_save_config(void);

// Odometer (wear-leveled, CRC-protected)
uint32_t hal_nvs_get_odometer(void);
void hal_nvs_save_odometer(uint32_t km);
void hal_nvs_save_meters(uint32_t meters);

#endif
