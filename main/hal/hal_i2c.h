#ifndef HAL_I2C_H
#define HAL_I2C_H

#include <stdint.h>
#include <stdbool.h>
#include "driver/i2c_master.h"

// Initialize I2C bus and devices (MCP23017, BH1750)
void hal_i2c_init(void);

// Get I2C bus handle (for CH422G backlight control)
i2c_master_bus_handle_t hal_i2c_get_bus(void);

// Read ambient light from BH1750 in lux (sub-lux precision with Mode2 + high MTreg)
float hal_i2c_read_light_lux(void);

// Read ignition state from MCP23017 (A5)
bool hal_i2c_read_ignition(void);

// Read all MCP23017 inputs (for future expansion)
uint16_t hal_i2c_read_inputs(void);

// Device status (true = responding)
bool hal_i2c_mcp_ok(void);
bool hal_i2c_bh1750_ok(void);

// Re-probe a failed device; returns true if it came back
bool hal_i2c_mcp_reprobe(void);
bool hal_i2c_bh1750_reprobe(void);

#endif
