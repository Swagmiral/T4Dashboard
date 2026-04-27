/**
 * NVS HAL — Dashboard config + Odometer storage
 *
 * Config:  JSON blob (merge on load) + optional one-time migration from legacy CRC blob
 * Odometer: wear-leveled rotating slots with CRC
 *
 * Both survive normal idf.py flash (only erase-flash wipes NVS).
 */

#include "hal_nvs.h"
#include "config.h"
#include "config_json.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_crc.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "nvs";

#define NS_ODOMETER   "odometer"
#define NS_CONFIG     "config"
#define CONFIG_KEY    "cfg"       /* legacy CRC blob */
#define CFG_JSON_KEY  "cfg_json"  /* merge-friendly JSON (no odometer) */
#define FUEL_LAST_PCT_KEY "fuel_last"

// ============================================================================
// Global config instance
// ============================================================================
dashboard_config_t g_config;

void config_set_defaults(dashboard_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->version = CONFIG_VERSION;

    cfg->voltage_multiplier     = 5.02f;
    cfg->voltage_ema_alpha      = 0.03f;
    cfg->voltage_settle_count   = 8;
    cfg->voltage_display_hyst   = 0.07f;

    cfg->color_red_enter_v      = 11.3f;
    cfg->color_red_exit_v       = 11.5f;
    cfg->color_yellow_enter_v   = 15.2f;
    cfg->color_yellow_exit_v    = 14.8f;

    cfg->warn_low_enter_v       = 11.0f;
    cfg->warn_low_exit_v        = 11.4f;
    cfg->warn_low_enter_ms      = 8000;
    cfg->warn_low_exit_ms       = 3000;
    cfg->warn_high_enter_v      = 15.2f;
    cfg->warn_high_exit_v       = 14.8f;
    cfg->warn_high_enter_ms     = 5000;
    cfg->warn_high_exit_ms      = 3000;

    // VW T4 fuel sender with 470Ω pull-up to 3.3V
    cfg->fuel_empty_v           = 1.15f;
    cfg->fuel_full_v            = 0.13f;
    cfg->fuel_emergency_pct     = 25;
    cfg->fuel_warn_on_pct       = 10;
    cfg->fuel_warn_off_pct      = 15;

    cfg->fuel_buf_size          = 20;
    cfg->fuel_sample_moving_ms  = 2000;
    cfg->fuel_sample_stopped_ms = 500;
    cfg->fuel_stop_delay_ms     = 30000;
    cfg->fuel_emergency_time_ms = 30000;
    cfg->fuel_hyst_threshold    = 1;
    cfg->fuel_hyst_cycles       = 3;
    cfg->fuel_nvs_save_cycles     = 10; /* NVS every N fuel gauge animations; 0 = shutdown only */
    cfg->fuel_no_read_timeout_ms   = 60000; /* 1 min after boot without fuel ADC → invalidate */

    cfg->temp_beta              = 3435.0f;
    cfg->temp_r_nominal         = 2500.0f;
    cfg->temp_min_c             = 40.0f;
    cfg->temp_max_c             = 120.0f;
    cfg->temp_overheat_pct      = 80;
    cfg->temp_overheat_delay_ms = 2000;

    cfg->temp_cold_on_pct       = 30;
    cfg->temp_cold_off_pct      = 33;
    cfg->temp_hot_on_pct        = 70;
    cfg->temp_hot_off_pct       = 67;

    cfg->light_ema_alpha        = 0.15f;
    cfg->brightness_min         = 15;
    cfg->brightness_max         = 255;
    cfg->temp_display_hyst      = 2;

    cfg->brightness_dark_lux    = 0.2f;
    cfg->brightness_bright_lux  = 0.7f;
    cfg->brightness_dark_pwm    = 70;
    cfg->brightness_bright_pwm  = 150;

    cfg->blink_on_ms            = 1000;
    cfg->blink_off_ms           = 500;

    cfg->pulses_per_km          = CONFIG_DEFAULT_PULSES_PER_KM;
    cfg->speed_max_kmh          = 180;
    cfg->speed_text_hyst        = 2;
    cfg->speed_text_interval_ms = 500;
    cfg->speed_slow_interval_ms = 1000;
    cfg->speed_slow_threshold   = 100;
    cfg->speed_arc_smooth       = 0.4f;
    cfg->speed_glitch_ns        = 1000;
    cfg->speed_min_period_us    = 1500;
    cfg->speed_window_ms        = 500;
    cfg->speed_stopped_ms       = 2000;

    cfg->tach_enabled           = 0;
    cfg->tach_pulses_per_rev    = 1;
    cfg->tach_max_rpm           = 6000;

    cfg->color_normal           = 0xFFFFFF;
    cfg->color_red              = 0xFF0814;
    cfg->color_green            = 0x00FF00;
    cfg->color_yellow           = 0xFFCC00;
    cfg->color_cold             = 0x00BFFF;
    cfg->color_high_beam        = 0x0066FF;
    cfg->color_glow_plug        = 0xFFAA00;
    cfg->color_dim              = 0x080808;
    cfg->color_disabled         = 0x080808;
    cfg->color_temp_ok          = 0xFFFFFF;
    cfg->color_fog              = 0x00FF00;
    cfg->color_wifi_active      = 0x0066FF;
    cfg->color_wifi_connected   = 0x00FF00;
    cfg->color_wifi_off         = 0x080808;
    cfg->color_bar_bg           = 0x080808;
    cfg->color_speedo_bg        = 0x1E1E1E;
    cfg->color_speedo_ind       = 0xFFFFFF;

    cfg->pin_ignition           = 0;
    cfg->pin_blinker_l          = 6;
    cfg->pin_blinker_r          = 5;
    cfg->pin_high_beam          = 3;
    cfg->pin_glow_plug          = 4;
    cfg->pin_brake              = 255;
    cfg->pin_oil                = 255;
    cfg->pin_fog_light          = 255;

    cfg->adc_voltage_ch         = 2;
    cfg->adc_fuel_ch            = 0;
    cfg->adc_temp_ch            = 1;

    cfg->i2c_scl_hz             = 100000;

    cfg->shutdown_delay_s       = 5;

    cfg->wifi_start_delay_s     = 10;
    cfg->wifi_idle_timeout_s    = 60;
    cfg->wifi_active_timeout_s  = 600;
    strncpy(cfg->wifi_ssid, "Dr.Agon", sizeof(cfg->wifi_ssid) - 1);
    strncpy(cfg->wifi_pass, "cykablyat", sizeof(cfg->wifi_pass) - 1);
}

void config_sanitize(dashboard_config_t *cfg)
{
    if (cfg->pulses_per_km == 0)
        cfg->pulses_per_km = CONFIG_DEFAULT_PULSES_PER_KM;
    if (cfg->temp_r_nominal < 1.0f)
        cfg->temp_r_nominal = 2500.0f;
    if (cfg->temp_beta < 1.0f)
        cfg->temp_beta = 3435.0f;
    if (cfg->tach_pulses_per_rev == 0)
        cfg->tach_pulses_per_rev = 1;
    if (cfg->voltage_multiplier < 0.01f)
        cfg->voltage_multiplier = 5.02f;
    if (cfg->speed_window_ms == 0)
        cfg->speed_window_ms = 500;
    if (cfg->speed_stopped_ms == 0)
        cfg->speed_stopped_ms = 2000;
    if (cfg->fuel_nvs_save_cycles > 5000)
        cfg->fuel_nvs_save_cycles = 5000;
    if (cfg->fuel_no_read_timeout_ms > 86400000u)
        cfg->fuel_no_read_timeout_ms = 86400000u;
    if (cfg->i2c_scl_hz < 50000u)
        cfg->i2c_scl_hz = 100000u;
    if (cfg->i2c_scl_hz > 1000000u)
        cfg->i2c_scl_hz = 1000000u;
}

// ============================================================================
// Config NVS (CRC-protected blob)
// ============================================================================
typedef struct {
    dashboard_config_t cfg;
    uint32_t crc32;
} config_nvs_t;

static nvs_handle_t s_config_handle;

static uint32_t config_calc_crc(const dashboard_config_t *cfg)
{
    return esp_crc32_le(0, (const uint8_t *)cfg, sizeof(*cfg));
}

/** One-time: legacy CRC blob -> cfg_json, then erase legacy key. */
static bool migrate_legacy_config_blob_to_json(void)
{
    config_nvs_t stored;
    size_t size = sizeof(stored);
    esp_err_t ret = nvs_get_blob(s_config_handle, CONFIG_KEY, &stored, &size);
    if (ret != ESP_OK || size != sizeof(stored))
        return false;

    if (config_calc_crc(&stored.cfg) != stored.crc32) {
        ESP_LOGW(TAG, "Legacy config CRC mismatch, skipping migration");
        return false;
    }

    memcpy(&g_config, &stored.cfg, sizeof(g_config));
    g_config.version = CONFIG_VERSION;
    config_apply_forced_defaults(&g_config);
    config_sanitize(&g_config);

    char *json = dashboard_config_to_json(false);
    if (!json) {
        ESP_LOGE(TAG, "Legacy migrate: JSON alloc failed");
        return true;
    }
    ret = nvs_set_blob(s_config_handle, CFG_JSON_KEY, json, strlen(json) + 1);
    free(json);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Legacy migrate: write cfg_json failed: %s", esp_err_to_name(ret));
        return true;
    }
    nvs_erase_key(s_config_handle, CONFIG_KEY);
    nvs_commit(s_config_handle);
    ESP_LOGI(TAG, "Migrated legacy config blob to cfg_json");
    return true;
}

bool hal_nvs_load_config(void)
{
    bool loaded_from_nvs = false;

    config_set_defaults(&g_config);

    const size_t cap = 20 * 1024;
    char *buf = malloc(cap + 1);
    if (buf) {
        size_t size = cap;
        esp_err_t ret = nvs_get_blob(s_config_handle, CFG_JSON_KEY, buf, &size);
        if (ret == ESP_OK && size > 0 && size <= cap) {
            buf[size] = '\0';
            if (dashboard_config_from_json(buf, false))
                loaded_from_nvs = true;
            else
                ESP_LOGW(TAG, "cfg_json parse failed");
        }
        free(buf);
    }

    if (!loaded_from_nvs && migrate_legacy_config_blob_to_json())
        loaded_from_nvs = true;

    config_sanitize(&g_config);

    if (loaded_from_nvs)
        ESP_LOGI(TAG, "Config loaded from NVS (v%u)", g_config.version);
    else
        ESP_LOGW(TAG, "No valid config in NVS, using defaults");

    return loaded_from_nvs;
}

bool hal_nvs_save_config(void)
{
    config_sanitize(&g_config);
    g_config.version = CONFIG_VERSION;

    char *json = dashboard_config_to_json(false);
    if (!json) {
        ESP_LOGE(TAG, "Config JSON alloc failed");
        return false;
    }

    esp_err_t ret = nvs_set_blob(s_config_handle, CFG_JSON_KEY, json, strlen(json) + 1);
    free(json);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write cfg_json: %s", esp_err_to_name(ret));
        return false;
    }

    nvs_erase_key(s_config_handle, CONFIG_KEY);
    nvs_commit(s_config_handle);
    ESP_LOGI(TAG, "Config saved to NVS (JSON)");
    return true;
}

// Number of redundant slots for wear leveling
#define NUM_SLOTS           4

// Slot key names
static const char *slot_keys[NUM_SLOTS] = {
    "odo_slot0",
    "odo_slot1", 
    "odo_slot2",
    "odo_slot3"
};

// Index of next slot to write
#define SLOT_INDEX_KEY      "slot_idx"

// Data structure stored in each slot
typedef struct {
    uint32_t kilometers;    // Odometer value in km
    uint32_t meters;        // Sub-km meters (0-999)
    uint32_t write_count;   // Number of times this slot was written
    uint32_t crc32;         // CRC32 of above fields
} odometer_slot_t;

static nvs_handle_t s_nvs_handle;
static uint8_t current_slot_index = 0;
static uint32_t cached_km = 0;
static uint32_t cached_meters = 0;

// Calculate CRC32 for a slot (excluding the CRC field itself)
static uint32_t calc_slot_crc(const odometer_slot_t *slot)
{
    return esp_crc32_le(0, (const uint8_t *)slot, 
                        sizeof(odometer_slot_t) - sizeof(uint32_t));
}

// Validate a slot's CRC
static bool validate_slot(const odometer_slot_t *slot)
{
    return calc_slot_crc(slot) == slot->crc32;
}

// Read a slot from NVS
static bool read_slot(uint8_t index, odometer_slot_t *slot)
{
    if (index >= NUM_SLOTS) return false;
    
    size_t size = sizeof(odometer_slot_t);
    esp_err_t ret = nvs_get_blob(s_nvs_handle, slot_keys[index], slot, &size);
    
    if (ret != ESP_OK || size != sizeof(odometer_slot_t)) {
        return false;
    }
    
    return validate_slot(slot);
}

// Write a slot to NVS
static bool write_slot(uint8_t index, odometer_slot_t *slot)
{
    if (index >= NUM_SLOTS) return false;
    
    // Calculate CRC before writing
    slot->crc32 = calc_slot_crc(slot);
    
    esp_err_t ret = nvs_set_blob(s_nvs_handle, slot_keys[index], slot, sizeof(odometer_slot_t));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write slot %d", index);
        return false;
    }
    
    return true;
}

void hal_nvs_init(void)
{
    ESP_LOGI(TAG, "Initializing NVS...");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Config namespace
    ret = nvs_open(NS_CONFIG, NVS_READWRITE, &s_config_handle);
    ESP_ERROR_CHECK(ret);

    // Odometer namespace
    ret = nvs_open(NS_ODOMETER, NVS_READWRITE, &s_nvs_handle);
    ESP_ERROR_CHECK(ret);

    ret = nvs_get_u8(s_nvs_handle, SLOT_INDEX_KEY, &current_slot_index);
    if (ret != ESP_OK) {
        current_slot_index = 0;
    }

    ESP_LOGI(TAG, "NVS initialized, slot index: %d", current_slot_index);
}

uint32_t hal_nvs_get_odometer(void)
{
    odometer_slot_t best_slot = {0};
    uint32_t best_write_count = 0;
    bool found_valid = false;
    
    // Find the most recent valid slot (highest write_count)
    for (int i = 0; i < NUM_SLOTS; i++) {
        odometer_slot_t slot;
        if (read_slot(i, &slot)) {
            if (!found_valid || slot.write_count > best_write_count) {
                best_slot = slot;
                best_write_count = slot.write_count;
                found_valid = true;
                ESP_LOGI(TAG, "Valid slot %d: %lu km, writes: %lu", 
                         i, slot.kilometers, slot.write_count);
            }
        } else {
            ESP_LOGW(TAG, "Slot %d invalid or empty", i);
        }
    }
    
    if (found_valid) {
        cached_km = best_slot.kilometers;
        cached_meters = best_slot.meters;
        ESP_LOGI(TAG, "Odometer loaded: %lu km + %lu m", cached_km, cached_meters);
        return cached_km;
    }
    
    // No valid data found - first boot or all corrupted
    ESP_LOGW(TAG, "No valid odometer data, starting at 0");
    cached_km = 0;
    cached_meters = 0;
    return 0;
}

void hal_nvs_save_odometer(uint32_t km)
{
    // Get previous write count from any valid slot
    uint32_t write_count = 0;
    for (int i = 0; i < NUM_SLOTS; i++) {
        odometer_slot_t slot;
        if (read_slot(i, &slot)) {
            if (slot.write_count > write_count) {
                write_count = slot.write_count;
            }
        }
    }
    write_count++;  // Increment for new write
    
    // Prepare slot data
    odometer_slot_t slot = {
        .kilometers = km,
        .meters = cached_meters,
        .write_count = write_count,
        .crc32 = 0  // Will be calculated in write_slot
    };
    
    // Write to current slot
    if (write_slot(current_slot_index, &slot)) {
        ESP_LOGI(TAG, "Saved to slot %d: %lu km (write #%lu)", 
                 current_slot_index, km, write_count);
        
        // Advance to next slot (wear leveling)
        current_slot_index = (current_slot_index + 1) % NUM_SLOTS;
        nvs_set_u8(s_nvs_handle, SLOT_INDEX_KEY, current_slot_index);
        
        // Commit to flash
        nvs_commit(s_nvs_handle);
    } else {
        ESP_LOGE(TAG, "Failed to save odometer!");
        
        // Try next slot
        current_slot_index = (current_slot_index + 1) % NUM_SLOTS;
        if (write_slot(current_slot_index, &slot)) {
            ESP_LOGI(TAG, "Saved to backup slot %d", current_slot_index);
            nvs_commit(s_nvs_handle);
        }
    }
    
    cached_km = km;
}

uint32_t hal_nvs_add_distance_m(uint32_t delta_m)
{
    if (delta_m == 0)
        return cached_km;

    uint32_t prev_km = cached_km;
    uint64_t total_m = (uint64_t)cached_km * 1000ULL + (uint64_t)cached_meters + (uint64_t)delta_m;
    cached_km = (uint32_t)(total_m / 1000);
    cached_meters = (uint32_t)(total_m % 1000);

    if (cached_km != prev_km)
        hal_nvs_save_odometer(cached_km);

    return cached_km;
}

bool hal_nvs_load_last_fuel_pct(uint8_t *out_pct)
{
    if (!out_pct)
        return false;
    uint8_t v = 255;
    esp_err_t err = nvs_get_u8(s_config_handle, FUEL_LAST_PCT_KEY, &v);
    if (err != ESP_OK)
        return false;
    if (v > 100)
        return false;
    *out_pct = v;
    return true;
}

void hal_nvs_save_last_fuel_pct(uint8_t pct)
{
    if (pct > 100)
        return;
    esp_err_t err = nvs_set_u8(s_config_handle, FUEL_LAST_PCT_KEY, pct);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "fuel_last save failed: %s", esp_err_to_name(err));
        return;
    }
    nvs_commit(s_config_handle);
}

void hal_nvs_erase_last_fuel_pct(void)
{
    esp_err_t err = nvs_erase_key(s_config_handle, FUEL_LAST_PCT_KEY);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND)
        ESP_LOGW(TAG, "fuel_last erase failed: %s", esp_err_to_name(err));
    nvs_commit(s_config_handle);
}
