/**
 * Dashboard config <-> JSON (HTTP backup + NVS merge storage).
 */

#include "config_json.h"
#include "config.h"
#include "hal_nvs.h"

#include "cJSON.h"
#include "esp_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "cfg_json";

extern volatile uint32_t g_odometer_km;

/*
 * Key names listed here are reset from firmware defaults after every JSON merge.
 * Implement each name in the loop below (see config_set_defaults in hal_nvs.c).
 */
static const char *const forced_default_keys[] = {
    NULL,
};

void config_apply_forced_defaults(dashboard_config_t *cfg)
{
    (void)cfg;
    for (const char *const *p = forced_default_keys; *p; p++)
        ESP_LOGW(TAG, "forced_default key not implemented: %s", *p);
}

static void add_f2(cJSON *j, const char *name, float v)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%.2f", (double)v);
    char *p = buf + strlen(buf) - 1;
    while (p > buf && *p == '0')
        p--;
    if (*p == '.')
        p--;
    p[1] = '\0';
    cJSON_AddItemToObject(j, name, cJSON_CreateRaw(buf));
}

#define ADD_F(j, name) add_f2(j, #name, g_config.name)
#define ADD_I(j, name) cJSON_AddNumberToObject(j, #name, g_config.name)

char *dashboard_config_to_json(bool include_odometer)
{
    cJSON *j = cJSON_CreateObject();
    if (!j)
        return NULL;

    cJSON_AddNumberToObject(j, "config_version", CONFIG_VERSION);
    if (include_odometer)
        cJSON_AddNumberToObject(j, "odometer_km", g_odometer_km);

    ADD_F(j, voltage_multiplier);
    ADD_F(j, voltage_ema_alpha);
    ADD_I(j, voltage_settle_count);
    ADD_F(j, voltage_display_hyst);

    ADD_F(j, color_red_enter_v);
    ADD_F(j, color_red_exit_v);
    ADD_F(j, color_yellow_enter_v);
    ADD_F(j, color_yellow_exit_v);

    ADD_F(j, warn_low_enter_v);
    ADD_F(j, warn_low_exit_v);
    ADD_I(j, warn_low_enter_ms);
    ADD_I(j, warn_low_exit_ms);
    ADD_F(j, warn_high_enter_v);
    ADD_F(j, warn_high_exit_v);
    ADD_I(j, warn_high_enter_ms);
    ADD_I(j, warn_high_exit_ms);

    ADD_F(j, fuel_empty_v);
    ADD_F(j, fuel_full_v);
    ADD_I(j, fuel_emergency_pct);
    ADD_I(j, fuel_warn_on_pct);
    ADD_I(j, fuel_warn_off_pct);

    ADD_I(j, fuel_buf_size);
    ADD_I(j, fuel_sample_moving_ms);
    ADD_I(j, fuel_sample_stopped_ms);
    ADD_I(j, fuel_stop_delay_ms);
    ADD_I(j, fuel_emergency_time_ms);
    ADD_I(j, fuel_hyst_threshold);
    ADD_I(j, fuel_hyst_cycles);
    ADD_I(j, fuel_nvs_save_cycles);
    ADD_I(j, fuel_no_read_timeout_ms);

    ADD_F(j, temp_beta);
    ADD_F(j, temp_r_nominal);
    ADD_F(j, temp_min_c);
    ADD_F(j, temp_max_c);
    ADD_I(j, temp_overheat_pct);
    ADD_I(j, temp_overheat_delay_ms);

    ADD_I(j, temp_cold_on_pct);
    ADD_I(j, temp_cold_off_pct);
    ADD_I(j, temp_hot_on_pct);
    ADD_I(j, temp_hot_off_pct);
    ADD_I(j, temp_display_hyst);

    ADD_F(j, light_ema_alpha);
    ADD_I(j, brightness_min);
    ADD_I(j, brightness_max);
    ADD_F(j, brightness_dark_lux);
    ADD_F(j, brightness_bright_lux);
    ADD_I(j, brightness_dark_pwm);
    ADD_I(j, brightness_bright_pwm);

    ADD_I(j, blink_on_ms);
    ADD_I(j, blink_off_ms);

    ADD_I(j, pulses_per_km);
    ADD_I(j, speed_max_kmh);
    ADD_I(j, speed_text_hyst);
    ADD_I(j, speed_text_interval_ms);
    ADD_I(j, speed_slow_interval_ms);
    ADD_I(j, speed_slow_threshold);
    ADD_F(j, speed_arc_smooth);
    ADD_I(j, speed_glitch_ns);
    ADD_I(j, speed_min_period_us);
    ADD_I(j, speed_window_ms);
    ADD_I(j, speed_stopped_ms);

    ADD_I(j, tach_enabled);
    ADD_I(j, tach_pulses_per_rev);
    ADD_I(j, tach_max_rpm);

    ADD_I(j, color_normal);
    ADD_I(j, color_red);
    ADD_I(j, color_green);
    ADD_I(j, color_yellow);
    ADD_I(j, color_cold);
    ADD_I(j, color_high_beam);
    ADD_I(j, color_glow_plug);
    ADD_I(j, color_dim);
    ADD_I(j, color_disabled);
    ADD_I(j, color_temp_ok);
    ADD_I(j, color_fog);
    ADD_I(j, color_wifi_active);
    ADD_I(j, color_wifi_connected);
    ADD_I(j, color_wifi_off);
    ADD_I(j, color_bar_bg);
    ADD_I(j, color_speedo_bg);
    ADD_I(j, color_speedo_ind);

    ADD_I(j, pin_ignition);
    ADD_I(j, pin_blinker_l);
    ADD_I(j, pin_blinker_r);
    ADD_I(j, pin_high_beam);
    ADD_I(j, pin_glow_plug);
    ADD_I(j, pin_brake);
    ADD_I(j, pin_oil);
    ADD_I(j, pin_fog_light);

    ADD_I(j, adc_voltage_ch);
    ADD_I(j, adc_fuel_ch);
    ADD_I(j, adc_temp_ch);

    ADD_I(j, i2c_scl_hz);

    ADD_I(j, shutdown_delay_s);

    cJSON_AddStringToObject(j, "wifi_ssid", g_config.wifi_ssid);
    cJSON_AddStringToObject(j, "wifi_pass", g_config.wifi_pass);
    ADD_I(j, wifi_start_delay_s);
    ADD_I(j, wifi_idle_timeout_s);
    ADD_I(j, wifi_active_timeout_s);

    char *str = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    return str;
}

#define JSON_SET_FLOAT(name)                                                                       \
    do {                                                                                           \
        cJSON *item = cJSON_GetObjectItem(j, #name);                                               \
        if (item && cJSON_IsNumber(item))                                                          \
            g_config.name = (float)item->valuedouble;                                              \
    } while (0)

#define JSON_SET_U8(name)                                                                          \
    do {                                                                                           \
        cJSON *item = cJSON_GetObjectItem(j, #name);                                               \
        if (item && cJSON_IsNumber(item))                                                          \
            g_config.name = (uint8_t)item->valuedouble;                                            \
    } while (0)

#define JSON_SET_U16(name)                                                                         \
    do {                                                                                           \
        cJSON *item = cJSON_GetObjectItem(j, #name);                                               \
        if (item && cJSON_IsNumber(item))                                                          \
            g_config.name = (uint16_t)item->valuedouble;                                           \
    } while (0)

#define JSON_SET_U32(name)                                                                         \
    do {                                                                                           \
        cJSON *item = cJSON_GetObjectItem(j, #name);                                               \
        if (item && cJSON_IsNumber(item))                                                          \
            g_config.name = (uint32_t)item->valuedouble;                                           \
    } while (0)

bool dashboard_config_from_json(const char *buf, bool apply_odometer)
{
    cJSON *j = cJSON_Parse(buf);
    if (!j) {
        ESP_LOGW(TAG, "JSON parse failed");
        return false;
    }

    if (apply_odometer) {
        cJSON *odo = cJSON_GetObjectItem(j, "odometer_km");
        if (odo && cJSON_IsNumber(odo)) {
            uint32_t new_km = (uint32_t)odo->valuedouble;
            g_odometer_km = new_km;
            hal_nvs_save_odometer(new_km);
        }
    }

    JSON_SET_FLOAT(voltage_multiplier);
    JSON_SET_FLOAT(voltage_ema_alpha);
    JSON_SET_U8(voltage_settle_count);
    JSON_SET_FLOAT(voltage_display_hyst);

    JSON_SET_FLOAT(color_red_enter_v);
    JSON_SET_FLOAT(color_red_exit_v);
    JSON_SET_FLOAT(color_yellow_enter_v);
    JSON_SET_FLOAT(color_yellow_exit_v);

    JSON_SET_FLOAT(warn_low_enter_v);
    JSON_SET_FLOAT(warn_low_exit_v);
    JSON_SET_U16(warn_low_enter_ms);
    JSON_SET_U16(warn_low_exit_ms);
    JSON_SET_FLOAT(warn_high_enter_v);
    JSON_SET_FLOAT(warn_high_exit_v);
    JSON_SET_U16(warn_high_enter_ms);
    JSON_SET_U16(warn_high_exit_ms);

    JSON_SET_FLOAT(fuel_empty_v);
    JSON_SET_FLOAT(fuel_full_v);
    JSON_SET_U8(fuel_emergency_pct);
    JSON_SET_U8(fuel_warn_on_pct);
    JSON_SET_U8(fuel_warn_off_pct);

    JSON_SET_U8(fuel_buf_size);
    JSON_SET_U16(fuel_sample_moving_ms);
    JSON_SET_U16(fuel_sample_stopped_ms);
    JSON_SET_U16(fuel_stop_delay_ms);
    JSON_SET_U16(fuel_emergency_time_ms);
    JSON_SET_U8(fuel_hyst_threshold);
    JSON_SET_U8(fuel_hyst_cycles);
    JSON_SET_U16(fuel_nvs_save_cycles);
    JSON_SET_U32(fuel_no_read_timeout_ms);

    JSON_SET_FLOAT(temp_beta);
    JSON_SET_FLOAT(temp_r_nominal);
    JSON_SET_FLOAT(temp_min_c);
    JSON_SET_FLOAT(temp_max_c);
    JSON_SET_U8(temp_overheat_pct);
    JSON_SET_U16(temp_overheat_delay_ms);

    JSON_SET_U8(temp_cold_on_pct);
    JSON_SET_U8(temp_cold_off_pct);
    JSON_SET_U8(temp_hot_on_pct);
    JSON_SET_U8(temp_hot_off_pct);
    JSON_SET_U8(temp_display_hyst);

    JSON_SET_FLOAT(light_ema_alpha);
    JSON_SET_U8(brightness_min);
    JSON_SET_U8(brightness_max);
    JSON_SET_FLOAT(brightness_dark_lux);
    JSON_SET_FLOAT(brightness_bright_lux);
    JSON_SET_U8(brightness_dark_pwm);
    JSON_SET_U8(brightness_bright_pwm);

    JSON_SET_U16(blink_on_ms);
    JSON_SET_U16(blink_off_ms);

    JSON_SET_U16(pulses_per_km);
    JSON_SET_U16(speed_max_kmh);
    JSON_SET_U8(speed_text_hyst);
    JSON_SET_U16(speed_text_interval_ms);
    JSON_SET_U16(speed_slow_interval_ms);
    JSON_SET_U16(speed_slow_threshold);
    JSON_SET_FLOAT(speed_arc_smooth);
    JSON_SET_U16(speed_glitch_ns);
    JSON_SET_U32(speed_min_period_us);
    JSON_SET_U16(speed_window_ms);
    JSON_SET_U16(speed_stopped_ms);

    JSON_SET_U8(tach_enabled);
    JSON_SET_U8(tach_pulses_per_rev);
    JSON_SET_U16(tach_max_rpm);

    JSON_SET_U32(color_normal);
    JSON_SET_U32(color_red);
    JSON_SET_U32(color_green);
    JSON_SET_U32(color_yellow);
    JSON_SET_U32(color_cold);
    JSON_SET_U32(color_high_beam);
    JSON_SET_U32(color_glow_plug);
    JSON_SET_U32(color_dim);
    JSON_SET_U32(color_disabled);
    JSON_SET_U32(color_temp_ok);
    JSON_SET_U32(color_fog);
    JSON_SET_U32(color_wifi_active);
    JSON_SET_U32(color_wifi_connected);
    JSON_SET_U32(color_wifi_off);
    JSON_SET_U32(color_bar_bg);
    JSON_SET_U32(color_speedo_bg);
    JSON_SET_U32(color_speedo_ind);

    JSON_SET_U8(pin_ignition);
    JSON_SET_U8(pin_blinker_l);
    JSON_SET_U8(pin_blinker_r);
    JSON_SET_U8(pin_high_beam);
    JSON_SET_U8(pin_glow_plug);
    JSON_SET_U8(pin_brake);
    JSON_SET_U8(pin_oil);
    JSON_SET_U8(pin_fog_light);

    JSON_SET_U8(adc_voltage_ch);
    JSON_SET_U8(adc_fuel_ch);
    JSON_SET_U8(adc_temp_ch);

    JSON_SET_U32(i2c_scl_hz);

    JSON_SET_U8(shutdown_delay_s);

    {
        cJSON *s = cJSON_GetObjectItem(j, "wifi_ssid");
        if (s && cJSON_IsString(s)) {
            strncpy(g_config.wifi_ssid, s->valuestring, sizeof(g_config.wifi_ssid) - 1);
            g_config.wifi_ssid[sizeof(g_config.wifi_ssid) - 1] = '\0';
        }
        s = cJSON_GetObjectItem(j, "wifi_pass");
        if (s && cJSON_IsString(s) && strlen(s->valuestring) >= 8) {
            strncpy(g_config.wifi_pass, s->valuestring, sizeof(g_config.wifi_pass) - 1);
            g_config.wifi_pass[sizeof(g_config.wifi_pass) - 1] = '\0';
        }
    }
    JSON_SET_U8(wifi_start_delay_s);
    JSON_SET_U8(wifi_idle_timeout_s);
    JSON_SET_U16(wifi_active_timeout_s);

    cJSON_Delete(j);

    g_config.version = CONFIG_VERSION;
    config_apply_forced_defaults(&g_config);
    config_sanitize(&g_config);
    return true;
}
