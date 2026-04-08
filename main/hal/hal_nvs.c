/**
 * NVS HAL — Dashboard config + Odometer storage
 *
 * Config:  single CRC-protected blob, versioned for migration
 * Odometer: wear-leveled rotating slots with CRC
 *
 * Both survive normal idf.py flash (only erase-flash wipes NVS).
 */

#include "hal_nvs.h"
#include "config.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_crc.h"
#include <string.h>

static const char *TAG = "nvs";

#define NS_ODOMETER   "odometer"
#define NS_CONFIG     "config"
#define CONFIG_KEY    "cfg"

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
    cfg->fuel_learned_empty_v   = -1.0f;
    cfg->fuel_learned_full_v    = -1.0f;
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

    cfg->pulses_per_km          = 539;
    cfg->speed_max_kmh          = 180;
    cfg->speed_text_interval_ms = 500;
    cfg->speed_slow_threshold   = 10;
    cfg->speed_slow_interval_ms = 1000;
    cfg->speed_text_hyst        = 2;
    cfg->speed_arc_smooth       = 0.4f;
    cfg->speed_filter_size      = 5;
    cfg->speed_max_accel        = 30;
    cfg->speed_confirm_count    = 3;

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

    cfg->shutdown_delay_s       = 5;

    cfg->wifi_start_delay_s     = 10;
    cfg->wifi_idle_timeout_s    = 60;
    cfg->wifi_active_timeout_s  = 600;
    strncpy(cfg->wifi_ssid, "Dr.Agon", sizeof(cfg->wifi_ssid) - 1);
    strncpy(cfg->wifi_pass, "cykablyat", sizeof(cfg->wifi_pass) - 1);
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

bool hal_nvs_load_config(void)
{
    config_set_defaults(&g_config);

    config_nvs_t stored;
    size_t size = sizeof(stored);
    esp_err_t ret = nvs_get_blob(s_config_handle, CONFIG_KEY, &stored, &size);

    if (ret != ESP_OK || size != sizeof(stored)) {
        ESP_LOGW(TAG, "No config in NVS, using defaults");
        return false;
    }

    if (config_calc_crc(&stored.cfg) != stored.crc32) {
        ESP_LOGE(TAG, "Config CRC mismatch, using defaults");
        return false;
    }

    if (stored.cfg.version != CONFIG_VERSION) {
        ESP_LOGW(TAG, "Config version %u != %u, using defaults",
                 stored.cfg.version, CONFIG_VERSION);
        return false;
    }

    memcpy(&g_config, &stored.cfg, sizeof(g_config));

    // Sanitize critical fields to prevent division-by-zero / math crashes
    if (g_config.pulses_per_km == 0)       g_config.pulses_per_km = 539;
    if (g_config.temp_r_nominal < 1.0f)    g_config.temp_r_nominal = 2500.0f;
    if (g_config.temp_beta < 1.0f)         g_config.temp_beta = 3435.0f;
    if (g_config.speed_filter_size < 1)    g_config.speed_filter_size = 5;
    if (g_config.speed_filter_size > 9)    g_config.speed_filter_size = 9;
    if (g_config.speed_confirm_count < 1)  g_config.speed_confirm_count = 3;
    if (g_config.tach_pulses_per_rev == 0) g_config.tach_pulses_per_rev = 1;
    if (g_config.voltage_multiplier < 0.01f) g_config.voltage_multiplier = 5.02f;

    ESP_LOGI(TAG, "Config loaded from NVS (v%u)", g_config.version);
    return true;
}

bool hal_nvs_save_config(void)
{
    g_config.version = CONFIG_VERSION;

    config_nvs_t blob;
    memcpy(&blob.cfg, &g_config, sizeof(g_config));
    blob.crc32 = config_calc_crc(&blob.cfg);

    esp_err_t ret = nvs_set_blob(s_config_handle, CONFIG_KEY, &blob, sizeof(blob));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write config: %s", esp_err_to_name(ret));
        return false;
    }
    nvs_commit(s_config_handle);
    ESP_LOGI(TAG, "Config saved to NVS");
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

void hal_nvs_save_meters(uint32_t meters)
{
    cached_meters = meters % 1000;
    
    // If we accumulated another km, save
    if (meters >= 1000) {
        cached_km += meters / 1000;
        hal_nvs_save_odometer(cached_km);
    }
}
