#ifndef HAL_DISPLAY_H
#define HAL_DISPLAY_H

#include <stdint.h>
#include <stdbool.h>
#include "driver/i2c_master.h"

// Initialize IO expander and turn off backlight immediately (call first!)
void hal_display_init_io_expander(i2c_master_bus_handle_t bus);

// Initialize display hardware
void hal_display_init(void);

// Enable backlight (call after display content is ready)
void hal_display_enable_backlight(i2c_master_bus_handle_t bus);

// Show splash screen (before LVGL init)
void hal_display_show_splash(void);

// Initialize LVGL
void hal_display_init_ui(void);

// LVGL tick (call from main loop ~every 10ms)
void hal_display_tick(void);

// Thread-safe LVGL access
bool hal_display_lock(int timeout_ms);
void hal_display_unlock(void);

/* Dashboard helpers: take the LVGL mutex internally — do not call while holding hal_display_lock(). */
void hal_display_set_voltage_valid(bool valid, float volts);
void hal_display_set_brightness(uint8_t level);
void hal_display_set_odometer(uint32_t km);

#endif
