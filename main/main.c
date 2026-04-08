/**
 * T4 Dashboard - Main Entry Point
 * VW T4 Digital Dashboard - FAST BOOT
 */

#include <stdio.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"

#include "config.h"
#include "hal_display.h"
#include "hal_i2c.h"
#include "hal_adc.h"
#include "hal_nvs.h"
#include "hal_speed.h"
#include "hal_tach.h"
#include "hal_wifi.h"
#include "dashboard.h"

static const char *TAG = "boot";

volatile uint32_t g_odometer_km = 0;

#define POWER_HOLD_PIN          GPIO_NUM_6
#define LIGHT_READ_INTERVAL_US  (500 * 1000)
#define SHUTDOWN_DELAY_US       ((int64_t)g_config.shutdown_delay_s * 1000 * 1000LL)

// NTC temperature sensor (P23 → 2.2kΩ pull-up to 3.3V → ADS1115 AIN1)
#define TEMP_PULLUP_OHM     2200.0f
#define TEMP_VREF           3.3f

void app_main(void)
{
    // === FIRST: latch power before caps discharge ===
    gpio_config_t pwr_cfg = {
        .pin_bit_mask = (1ULL << POWER_HOLD_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&pwr_cfg);
    gpio_set_level(POWER_HOLD_PIN, 1);

    int64_t t_start = esp_timer_get_time();

    // === NVS + config first (needed before display/ADC use calibration values) ===
    hal_nvs_init();
    hal_nvs_load_config();
    ESP_LOGW(TAG, "nvs: %lld ms", (esp_timer_get_time() - t_start) / 1000);

    // === CRITICAL PATH: Display ASAP ===
    hal_i2c_init();
    ESP_LOGW(TAG, "i2c: %lld ms", (esp_timer_get_time() - t_start) / 1000);

    hal_display_init_io_expander(hal_i2c_get_bus());
    if (!hal_i2c_mcp_ok()) {
        ESP_LOGW(TAG, "MCP23017 retry after IO expander init...");
        hal_i2c_mcp_reprobe();
    }
    hal_display_init();
    ESP_LOGW(TAG, "disp: %lld ms  mcp=%d", (esp_timer_get_time() - t_start) / 1000, hal_i2c_mcp_ok());

    hal_display_init_ui();
    ESP_LOGW(TAG, "lvgl: %lld ms", (esp_timer_get_time() - t_start) / 1000);

    if (hal_display_lock(100)) {
        dashboard_init();
        hal_display_unlock();
    }
    ESP_LOGW(TAG, "ui: %lld ms", (esp_timer_get_time() - t_start) / 1000);

    hal_display_tick();
    if (hal_display_lock(100)) {
        dashboard_start();
        hal_display_unlock();
    }
    ESP_LOGW(TAG, "render: %lld ms", (esp_timer_get_time() - t_start) / 1000);

    hal_display_enable_backlight(hal_i2c_get_bus());
    ESP_LOGW(TAG, "TOTAL: %lld ms  I2C: mcp=%d bh=%d  SDA=%d SCL=%d",
             (esp_timer_get_time() - t_start) / 1000,
             hal_i2c_mcp_ok(), hal_i2c_bh1750_ok(),
             gpio_get_level(8), gpio_get_level(9));

    // === Non-critical init ===
    hal_adc_init(hal_i2c_get_bus());
    if (g_config.adc_voltage_ch != 255)
        hal_adc_start_conversion(g_config.adc_voltage_ch);

    // Stop all log output so UART0 TX releases GPIO43/44 for speed/tach
    esp_log_level_set("*", ESP_LOG_NONE);

    hal_speed_init();
    if (g_config.tach_enabled)
        hal_tach_init();
    hal_wifi_init();

    g_odometer_km = hal_nvs_get_odometer();
    
    hal_display_set_odometer(g_odometer_km);

    if (hal_display_lock(100)) {
        dashboard_set_speed(0);
        dashboard_set_rpm(0);
        hal_display_unlock();
    }
    
    // === Main loop ===
    bool prev_left = false, prev_right = false;
    bool prev_high_beam = false, prev_glow_plug = false;
    int64_t blinker_l_settle = 0, blinker_r_settle = 0;

    float light_ema = -1.0f;
    int64_t next_light_read = 0;
    int16_t brightness_current = -1;
    int16_t brightness_target = -1;

    int64_t shutdown_at = 0;
    uint8_t shutdown_attempts = 0;

    // ADC channel rotation: 0 = voltage (AIN2), 1 = fuel (AIN0), 2 = temp (AIN1)
    uint8_t adc_phase = 0;

    bool fuel_config_dirty = false;
    uint8_t adc_settle[3] = {0, 0, 0};
    live_sensors_t sensors = {0};

    while (1) {
        int64_t now_us = esp_timer_get_time();

        // --- Read all MCP23017 inputs once ---
        uint16_t inputs = hal_i2c_read_inputs();
        {
            static int64_t next_mcp_log = 0;
            if (now_us >= next_mcp_log) {
                next_mcp_log = now_us + 1000000;
                ESP_LOGW("mcp", "inputs=0x%04X ign=%d", inputs, !(inputs & (1 << g_config.pin_ignition)));
            }
        }

        // Helper: read active-low input by config pin, 255 = disabled
        #define PIN_READ(pin, default_val) \
            ((pin) == 255 ? (default_val) : !(inputs & (1 << (pin))))

        // --- Ignition & delayed shutdown ---
        bool ignition = PIN_READ(g_config.pin_ignition, true);

        if (!ignition) {
            if (shutdown_at == 0 && shutdown_attempts < 2) {
                shutdown_at = now_us + SHUTDOWN_DELAY_US;
                ESP_LOGW(TAG, "Ignition OFF - shutting down in %ds", g_config.shutdown_delay_s);
            } else if (shutdown_at != 0 && now_us >= shutdown_at) {
                shutdown_attempts++;
                ESP_LOGW(TAG, "Saving data and powering off (attempt %d)...", shutdown_attempts);
                hal_nvs_save_odometer(g_odometer_km);
                if (fuel_config_dirty) hal_nvs_save_config();
                vTaskDelay(pdMS_TO_TICKS(50));
                gpio_set_level(POWER_HOLD_PIN, 0);
                vTaskDelay(pdMS_TO_TICKS(1000));
                shutdown_at = 0;
            }
        } else if (shutdown_at != 0 || shutdown_attempts > 0) {
            ESP_LOGW(TAG, "Ignition back ON - shutdown cancelled");
            shutdown_at = 0;
            shutdown_attempts = 0;
        }

        // --- Simulation flags ---
        uint16_t sf = hal_wifi_get_sim_flags();
        if (hal_display_lock(10)) {
            dashboard_set_sim_flags(sf);
            hal_display_unlock();
        }

        // --- Blinkers (independent settle) ---
        bool left  = PIN_READ(g_config.pin_blinker_l, false) || (sf & SIM_BLINKER_L);
        bool right = PIN_READ(g_config.pin_blinker_r, false) || (sf & SIM_BLINKER_R);

        if (left != prev_left) {
            if (blinker_l_settle == 0) {
                blinker_l_settle = now_us + 5000;
            } else if (now_us >= blinker_l_settle) {
                prev_left = left;
                blinker_l_settle = 0;
                if (hal_display_lock(10)) {
                    dashboard_set_turn_left(left);
                    hal_display_unlock();
                }
            }
        } else {
            blinker_l_settle = 0;
        }

        if (right != prev_right) {
            if (blinker_r_settle == 0) {
                blinker_r_settle = now_us + 5000;
            } else if (now_us >= blinker_r_settle) {
                prev_right = right;
                blinker_r_settle = 0;
                if (hal_display_lock(10)) {
                    dashboard_set_turn_right(right);
                    hal_display_unlock();
                }
            }
        } else {
            blinker_r_settle = 0;
        }

        // --- High beam & glow plug ---
        bool high_beam = PIN_READ(g_config.pin_high_beam, false) || (sf & SIM_HIGH_BEAM);
        bool glow_plug = PIN_READ(g_config.pin_glow_plug, false) || (sf & SIM_GLOW_PLUG);

        if (high_beam != prev_high_beam) {
            prev_high_beam = high_beam;
            if (hal_display_lock(10)) {
                dashboard_set_high_beam(high_beam);
                hal_display_unlock();
            }
        }

        if (glow_plug != prev_glow_plug) {
            prev_glow_plug = glow_plug;
            if (hal_display_lock(10)) {
                dashboard_set_glow_plug(glow_plug);
                hal_display_unlock();
            }
        }

        // --- Speed + odometer ---
        uint16_t speed_kmh = hal_speed_get_kmh();
        if (hal_display_lock(10)) {
            dashboard_set_speed(speed_kmh);
            hal_display_unlock();
        }

        if (speed_kmh == 0) {
            hal_speed_reset_distance();
        }
        uint32_t dist_m = hal_speed_get_distance_m();
        if (speed_kmh > 0 && dist_m >= 100) {
            g_odometer_km += dist_m / 1000;
            hal_nvs_save_meters(dist_m);
            hal_speed_reset_distance();
            if (hal_display_lock(10)) {
                dashboard_set_odometer(g_odometer_km);
                hal_display_unlock();
            }
        }

        // --- Tachometer ---
        if (g_config.tach_enabled) {
            uint16_t rpm = hal_tach_get_rpm();
            if (hal_display_lock(10)) {
                dashboard_set_rpm(rpm);
                hal_display_unlock();
            }
        }

        // --- WiFi icon ---
        {
            static uint8_t prev_wifi_state = 0xFF;
            uint8_t ws = (uint8_t)hal_wifi_get_state();
            if (ws != prev_wifi_state) {
                prev_wifi_state = ws;
                if (hal_display_lock(10)) {
                    dashboard_set_wifi_state(ws);
                    hal_display_unlock();
                }
            }
        }

        // --- ADC: cycle through enabled channels ---
        // Phase map: 0=voltage, 1=fuel, 2=temp
        uint8_t adc_ch_map[] = {g_config.adc_voltage_ch,
                                g_config.adc_fuel_ch,
                                g_config.adc_temp_ch};

        float v_adc = 0.0f;
        if (adc_ch_map[adc_phase] != 255 && hal_adc_read_raw(&v_adc)) {
            if (adc_settle[adc_phase] < 2) {
                adc_settle[adc_phase]++;
            } else if (adc_phase == 0) {
                float voltage = v_adc * g_config.voltage_multiplier;
                sensors.voltage = voltage;
                if (voltage >= 5.0f && voltage <= 24.0f) {
                    hal_display_set_voltage_valid(true, voltage);
                }
            } else if (adc_phase == 1) {
                float empty_v = (g_config.fuel_learned_empty_v > 0) ?
                                 g_config.fuel_learned_empty_v : g_config.fuel_empty_v;
                float full_v  = (g_config.fuel_learned_full_v > 0) ?
                                 g_config.fuel_learned_full_v : g_config.fuel_full_v;

                if (speed_kmh > 0) {
                    if (v_adc > empty_v) {
                        g_config.fuel_learned_empty_v = v_adc;
                        empty_v = v_adc;
                        fuel_config_dirty = true;
                        ESP_LOGW("fuel", "Learned EMPTY: %.3fV", v_adc);
                    } else if (v_adc < full_v) {
                        g_config.fuel_learned_full_v = v_adc;
                        full_v = v_adc;
                        fuel_config_dirty = true;
                        ESP_LOGW("fuel", "Learned FULL: %.3fV", v_adc);
                    }
                }

                float range = empty_v - full_v;
                uint8_t fuel_pct = 0;
                if (range > 0.001f) {
                    float t = (empty_v - v_adc) / range;
                    if (t < 0.0f) t = 0.0f;
                    if (t > 1.0f) t = 1.0f;
                    fuel_pct = (uint8_t)(t * 100.0f + 0.5f);
                }

                sensors.fuel_v = v_adc;
                sensors.fuel_pct = fuel_pct;

                if (hal_display_lock(10)) {
                    dashboard_feed_fuel(fuel_pct, speed_kmh);
                    hal_display_unlock();
                }
            } else {
                if (v_adc > 0.01f && v_adc < (TEMP_VREF - 0.01f)
                    && g_config.temp_r_nominal > 0.1f
                    && g_config.temp_beta > 0.1f) {
                    float r_ntc = TEMP_PULLUP_OHM * v_adc / (TEMP_VREF - v_adc);
                    float t_k = 1.0f / (1.0f / 298.15f +
                                logf(r_ntc / g_config.temp_r_nominal) / g_config.temp_beta);
                    float t_c = t_k - 273.15f;
                    if (t_c != t_c) t_c = 0.0f; // NaN guard

                    float range_c = g_config.temp_max_c - g_config.temp_min_c;
                    float pct = (range_c > 0.1f) ?
                        (t_c - g_config.temp_min_c) * 100.0f / range_c : 50.0f;
                    if (pct < 0.0f) pct = 0.0f;
                    if (pct > 100.0f) pct = 100.0f;

                    sensors.temp_c = t_c;
                    sensors.temp_pct = (uint8_t)(pct + 0.5f);

                    if (hal_display_lock(10)) {
                        dashboard_feed_temp(sensors.temp_pct);
                        hal_display_unlock();
                    }
                }
            }
        }

        // Advance to next enabled channel
        for (int i = 0; i < 3; i++) {
            adc_phase = (adc_phase + 1) % 3;
            if (adc_ch_map[adc_phase] != 255) break;
        }
        if (adc_ch_map[adc_phase] != 255) {
            hal_adc_start_conversion(adc_ch_map[adc_phase]);
        }

        // --- Auto-brightness (every 500ms) ---
        if (now_us >= next_light_read) {
            next_light_read = now_us + LIGHT_READ_INTERVAL_US;

            float lux = hal_i2c_read_light_lux();

            if (light_ema < 0.0f) {
                light_ema = lux;
            } else {
                light_ema += g_config.light_ema_alpha * (lux - light_ema);
            }

            float ldark = g_config.brightness_dark_lux;
            float lbright = g_config.brightness_bright_lux;
            int16_t pwm_dark = g_config.brightness_dark_pwm;
            int16_t pwm_bright = g_config.brightness_bright_pwm;
            int16_t pwm_max = g_config.brightness_max;

            if (light_ema < ldark) {
                brightness_target = pwm_dark;
            } else if (light_ema < lbright) {
                float t = (light_ema - ldark) / (lbright - ldark);
                brightness_target = pwm_dark + (int16_t)(t * (pwm_bright - pwm_dark));
            } else {
                float range = lbright > 0.01f ? lbright : 1.0f;
                float t = (light_ema - lbright) / range;
                if (t > 1.0f) t = 1.0f;
                brightness_target = pwm_bright + (int16_t)(t * (pwm_max - pwm_bright));
            }

            if (brightness_current < 0) {
                brightness_current = brightness_target;
                hal_display_set_brightness((uint8_t)brightness_current);
            }

            ESP_LOGW("light", "lux=%.2f  ema=%.2f  brightness=%d->%d",
                     lux, light_ema, brightness_current, brightness_target);
        }

        // Smooth fade: step 1 unit every 50ms (rate-limited to avoid I2C pressure)
        {
            static int64_t next_fade = 0;
            if (brightness_current >= 0 && brightness_current != brightness_target
                && now_us >= next_fade) {
                next_fade = now_us + 50000;
                if (brightness_current < brightness_target) brightness_current++;
                else brightness_current--;
                hal_display_set_brightness((uint8_t)brightness_current);
            }
        }

        // --- Push live sensor data to WiFi endpoint ---
        sensors.speed_kmh = speed_kmh;
        sensors.rpm = g_config.tach_enabled ? hal_tach_get_rpm() : 0;
        sensors.mcp_inputs = inputs;
        sensors.mcp_ok = hal_i2c_mcp_ok();
        sensors.ignition = ignition;
        if (brightness_current >= 0)
            sensors.brightness = (uint8_t)brightness_current;
        if (light_ema >= 0.0f)
            sensors.lux = light_ema;
        hal_wifi_update_sensors(&sensors);

        hal_display_tick();
        vTaskDelay(pdMS_TO_TICKS(8));
    }
}
