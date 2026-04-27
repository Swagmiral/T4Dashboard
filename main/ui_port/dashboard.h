#ifndef DASHBOARD_H
#define DASHBOARD_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize (call after ui_init)
void dashboard_init(void);

// Main indicators
void dashboard_set_speed(uint16_t kmh);
void dashboard_set_rpm(uint16_t rpm);
void dashboard_set_fuel(uint8_t percent);
/** Show last saved fuel % immediately at boot (before ADC smoothing). */
void dashboard_restore_fuel_from_nvs(uint8_t pct);
/** No valid fuel sensor: "---", reset smoother, neutral styling (NVS cleared by caller). */
void dashboard_fuel_invalidate_reading(void);
/** 0–100 for NVS persist, or 255 if nothing sensible to save yet. */
uint8_t dashboard_get_fuel_persist_pct(void);
void dashboard_feed_fuel(uint8_t raw_pct, uint16_t speed);
void dashboard_feed_temp(uint8_t raw_pct);
void dashboard_set_temp_ok(bool ok);
void dashboard_set_temp(int8_t celsius);
void dashboard_set_temp_level(uint8_t percent);  // COLD/OK/HOT with moving label
void dashboard_set_voltage_valid(bool valid, float volts);
void dashboard_feed_voltage(bool valid, float raw_volts);
void dashboard_set_sensor_error(uint8_t channel, bool error);
void dashboard_set_odometer(uint32_t km);

// Indicators
void dashboard_set_turn_left(bool on);
void dashboard_set_turn_right(bool on);
void dashboard_set_high_beam(bool on);
void dashboard_set_fog_light(bool on);
void dashboard_set_oil_warning(bool on);
void dashboard_set_brake_warning(bool on);
void dashboard_set_glow_plug(bool on);
void dashboard_set_battery_warning(bool on);
void dashboard_set_wifi_state(uint8_t state);  // 0=off, 1=active, 2=connected

// Warning popup
void dashboard_show_warning(const char* text);
void dashboard_hide_warning(void);

// Tachometer LED (0-6 segments)
void dashboard_set_tach_level(uint8_t level);

// Simulation override (session-only, from WiFi config page)
void dashboard_set_sim_flags(uint16_t flags);

// Timer for external input polling
void dashboard_start(void);
void dashboard_stop(void);

/** Feed smoothed HAL samples (call from main loop under display lock) */
void dashboard_process_hal_samples(uint16_t speed_kmh, uint8_t fuel_pct,
                                   uint8_t temp_smooth_val, bool voltage_valid, float voltage_raw);

#ifdef __cplusplus
}
#endif

#endif