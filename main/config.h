#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>
#include <stdbool.h>

#define CONFIG_VERSION 14

typedef struct {
    uint16_t version;

    // Voltage measurement
    float voltage_multiplier;       // divider ratio (default 5.02)
    float voltage_ema_alpha;        // EMA smoothing (default 0.03)
    uint8_t voltage_settle_count;   // samples before first display (default 8)
    float voltage_display_hyst;     // display rounding dead-zone in V (default 0.07)

    // Voltage color hysteresis (immediate, no time delay)
    float color_red_enter_v;        // enter red below this  (default 11.3)
    float color_red_exit_v;         // exit  red above this  (default 11.5)
    float color_yellow_enter_v;     // enter yellow above    (default 15.2)
    float color_yellow_exit_v;      // exit  yellow below    (default 14.8)

    // Voltage warning thresholds (with time delay)
    float warn_low_enter_v;         // low-voltage trigger   (default 11.0)
    float warn_low_exit_v;          // low-voltage clear     (default 11.4)
    uint16_t warn_low_enter_ms;     // activation delay      (default 8000)
    uint16_t warn_low_exit_ms;      // deactivation delay    (default 3000)
    float warn_high_enter_v;        // high-voltage trigger  (default 15.2)
    float warn_high_exit_v;         // high-voltage clear    (default 14.8)
    uint16_t warn_high_enter_ms;    // activation delay      (default 5000)
    uint16_t warn_high_exit_ms;     // deactivation delay    (default 3000)

    // Fuel gauge calibration
    float fuel_empty_v;             // ADC voltage at empty  (default 1.15)
    float fuel_full_v;              // ADC voltage at full   (default 0.13)
    float fuel_learned_empty_v;     // self-calibrated empty (default -1 = not learned)
    float fuel_learned_full_v;      // self-calibrated full  (default -1 = not learned)
    uint8_t fuel_emergency_pct;     // low-fuel warning %    (default 25)
    uint8_t fuel_warn_on_pct;       // show Low Fuel below   (default 10)
    uint8_t fuel_warn_off_pct;      // hide Low Fuel above   (default 15)

    // Fuel smoothing system
    uint8_t fuel_buf_size;          // averaging buffer size, max 30 (default 20)
    uint16_t fuel_sample_moving_ms; // sample interval while moving (default 2000)
    uint16_t fuel_sample_stopped_ms;// sample interval while stopped (default 500)
    uint16_t fuel_stop_delay_ms;    // delay before "stopped" mode (default 30000)
    uint16_t fuel_emergency_time_ms;// emergency override time (default 30000)
    uint8_t fuel_hyst_threshold;    // hysteresis % (default 1)
    uint8_t fuel_hyst_cycles;       // confirmation cycles (default 3)

    // Temperature sensor calibration
    float temp_beta;                // NTC beta coefficient  (default 3435)
    float temp_r_nominal;           // NTC resistance at 25°C (default 2500)
    float temp_min_c;               // gauge 0% temperature  (default 40)
    float temp_max_c;               // gauge 100% temperature (default 120)
    uint8_t temp_overheat_pct;      // overheat threshold %  (default 80)
    uint16_t temp_overheat_delay_ms;// confirmation time     (default 2000)

    // Temperature display thresholds (gauge %)
    uint8_t temp_cold_on_pct;       // enter COLD from OK    (default 30)
    uint8_t temp_cold_off_pct;      // exit COLD to OK       (default 33)
    uint8_t temp_hot_on_pct;        // enter HOT from OK     (default 70)
    uint8_t temp_hot_off_pct;       // exit HOT to OK        (default 67)
    uint8_t temp_display_hyst;      // display jitter guard % (default 2)

    // Auto-brightness
    float light_ema_alpha;          // light sensor EMA      (default 0.15)
    uint8_t brightness_min;         // floor (never darker)  (default 15)
    uint8_t brightness_max;         // ceiling               (default 255)
    float brightness_dark_lux;      // dark threshold lux    (default 0.2)
    float brightness_bright_lux;    // bright threshold lux  (default 0.7)
    uint8_t brightness_dark_pwm;    // PWM value at dark     (default 70)
    uint8_t brightness_bright_pwm;  // PWM value at bright   (default 150)

    // Warning blink timing
    uint16_t blink_on_ms;           // visible phase         (default 1000)
    uint16_t blink_off_ms;          // hidden phase          (default 500)

    // Speed sensor
    uint16_t pulses_per_km;         // VSS pulses per km     (default 539)
    uint16_t speed_max_kmh;         // arc max speed         (default 180)
    uint16_t speed_text_interval_ms;// label update interval (default 500)
    uint16_t speed_slow_threshold;  // below this → slower   (default 10)
    uint16_t speed_slow_interval_ms;// interval below thresh (default 1000)
    uint8_t speed_text_hyst;        // label hysteresis km/h (default 2)
    float speed_arc_smooth;         // arc EMA alpha 0-1     (default 0.4)
    uint8_t speed_filter_size;      // median filter depth 1-9 (default 5)
    uint8_t speed_max_accel;        // max km/h per second, 0=off (default 30)
    uint8_t speed_confirm_count;    // consecutive samples to override (default 3)

    // Tachometer
    uint8_t tach_enabled;           // 0=off, 1=on           (default 0)
    uint8_t tach_pulses_per_rev;    // pulses per revolution (default 1)
    uint16_t tach_max_rpm;          // display max RPM       (default 6000)

    // Power
    uint8_t shutdown_delay_s;       // seconds after ignition off (default 5)

    // Colors (0xRRGGBB)
    uint32_t color_normal;          // OK / default state        (default 0xFFFFFF)
    uint32_t color_red;             // warning / danger / HOT    (default 0xFF0814)
    uint32_t color_green;           // blinkers / fog / tach     (default 0x00FF00)
    uint32_t color_yellow;          // low fuel / caution        (default 0xFFCC00)
    uint32_t color_cold;            // temperature COLD          (default 0x00BFFF)
    uint32_t color_high_beam;       // high beam indicator       (default 0x0066FF)
    uint32_t color_glow_plug;       // glow plug indicator       (default 0xFFAA00)
    uint32_t color_dim;             // inactive icons             (default 0x080808)
    uint32_t color_disabled;        // disabled functions          (default 0x080808)
    uint32_t color_temp_ok;         // temperature OK state        (default 0xFFFFFF)
    uint32_t color_fog;             // fog light indicator         (default 0x00FF00)
    uint32_t color_wifi_active;     // WiFi on, no client       (default 0x0066FF)
    uint32_t color_wifi_connected;  // WiFi client connected    (default 0x00FF00)
    uint32_t color_wifi_off;        // WiFi off                 (default 0x080808)
    uint32_t color_bar_bg;          // fuel/temp bar background (default 0x080808)
    uint32_t color_speedo_bg;       // speedo grid tint         (default 0x1E1E1E)
    uint32_t color_speedo_ind;      // speedo arc indicator     (default 0xFFFFFF)

    // Input pin mapping (MCP23017 bit 0-15, 255 = disabled)
    uint8_t pin_ignition;           // default 0  (A0)
    uint8_t pin_blinker_l;          // default 6  (A6)
    uint8_t pin_blinker_r;          // default 5  (A5)
    uint8_t pin_high_beam;          // default 3  (A3)
    uint8_t pin_glow_plug;          // default 4  (A4)
    uint8_t pin_brake;              // default 255 (not wired)
    uint8_t pin_oil;                // default 255 (not wired)
    uint8_t pin_fog_light;          // default 255 (not wired)

    // ADC channel mapping (0-3, 255 = disabled)
    uint8_t adc_voltage_ch;         // default 2  (AIN2)
    uint8_t adc_fuel_ch;            // default 0  (AIN0)
    uint8_t adc_temp_ch;            // default 1  (AIN1)

    // WiFi
    uint8_t wifi_start_delay_s;     // seconds after boot        (default 10)
    uint8_t wifi_idle_timeout_s;    // auto-off if no client     (default 60)
    uint16_t wifi_active_timeout_s; // auto-off after last disconnect (default 600)
    char wifi_ssid[32];             // AP name                   (default "Dr.Agon")
    char wifi_pass[32];             // AP password, min 8 chars  (default "cykablyat")
} dashboard_config_t;

extern dashboard_config_t g_config;

void config_set_defaults(dashboard_config_t *cfg);

#endif
