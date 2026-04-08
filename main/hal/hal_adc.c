/**
 * ADS1115 HAL — multi-channel ADC via I2C
 *
 * Channels:
 *   AIN0 — fuel level sender (470Ω pull-up to 3.3V)
 *   AIN2 — battery voltage (100k/26k divider)
 *
 * ADS1115 ADDR pin to GND → I2C address 0x48
 */

#include "hal_adc.h"
#include "esp_log.h"
#include "driver/i2c_master.h"

static const char *TAG = "adc";

#define ADS1115_ADDR            0x48
#define ADS1115_REG_CONVERSION  0x00
#define ADS1115_REG_CONFIG      0x01
#define ADS1115_LSB_MV          0.125f

/*
 * Config register base (16-bit, MSB first):
 *   [15]    OS     = 1   (start single conversion)
 *   [14:12] MUX    = set per channel
 *   [11:9]  PGA    = 001 (±4.096V, LSB = 0.125mV)
 *   [8]     MODE   = 1   (single-shot)
 *   [7:5]   DR     = 100 (128 SPS)
 *   [4:0]   COMP   = 00011 (disabled)
 *
 * MUX values for single-ended: AIN0=100, AIN1=101, AIN2=110, AIN3=111
 */
static const uint8_t mux_bits[] = { 0x04, 0x05, 0x06, 0x07 };

static i2c_master_dev_handle_t ads_dev = NULL;
static bool ads_ok = false;
static uint8_t ads_fail_cnt = 0;
#define ADS_FAIL_LIMIT 3

void hal_adc_init(i2c_master_bus_handle_t bus)
{
    if (bus == NULL) {
        ESP_LOGE(TAG, "I2C bus is NULL");
        return;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ADS1115_ADDR,
        .scl_speed_hz = 100000,
    };
    esp_err_t ret = i2c_master_bus_add_device(bus, &dev_cfg, &ads_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add ADS1115: %s", esp_err_to_name(ret));
        return;
    }

    uint8_t reg = ADS1115_REG_CONFIG;
    uint8_t probe[2];
    ret = i2c_master_transmit_receive(ads_dev, &reg, 1, probe, 2, 10);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ADS1115 not responding (%s) - ADC disabled", esp_err_to_name(ret));
        return;
    }
    ads_ok = true;
    ESP_LOGI(TAG, "ADS1115 initialized at 0x%02X", ADS1115_ADDR);
}

bool hal_adc_start_conversion(uint8_t channel)
{
    if (!ads_ok || channel > 3) return false;

    uint8_t msb = 0x80 | (mux_bits[channel] << 4) | 0x03; // OS=1, MUX, PGA=001, MODE=1
    uint8_t lsb = 0x83; // DR=128SPS, comparator disabled
    uint8_t cfg[3] = { ADS1115_REG_CONFIG, msb, lsb };
    if (i2c_master_transmit(ads_dev, cfg, sizeof(cfg), 10) != ESP_OK) {
        if (++ads_fail_cnt >= ADS_FAIL_LIMIT) {
            ads_ok = false;
            ESP_LOGW(TAG, "ADS1115 lost - ADC disabled");
        }
        return false;
    }
    ads_fail_cnt = 0;
    return true;
}

bool hal_adc_read_raw(float *v_adc_out)
{
    if (v_adc_out == NULL || !ads_ok) return false;

    uint8_t reg = ADS1115_REG_CONVERSION;
    uint8_t data[2] = {0};
    esp_err_t ret = i2c_master_transmit_receive(ads_dev, &reg, 1, data, 2, 10);
    if (ret != ESP_OK) {
        if (++ads_fail_cnt >= ADS_FAIL_LIMIT) {
            ads_ok = false;
            ESP_LOGW(TAG, "ADS1115 lost - ADC disabled");
        }
        return false;
    }
    ads_fail_cnt = 0;

    int16_t raw = (int16_t)((data[0] << 8) | data[1]);
    if (raw < 0) return false;

    *v_adc_out = raw * (ADS1115_LSB_MV / 1000.0f);
    return true;
}

bool hal_adc_ok(void) { return ads_ok; }

bool hal_adc_reprobe(void)
{
    if (ads_ok || ads_dev == NULL) return ads_ok;
    uint8_t reg = ADS1115_REG_CONFIG;
    uint8_t probe[2];
    esp_err_t ret = i2c_master_transmit_receive(ads_dev, &reg, 1, probe, 2, 10);
    if (ret == ESP_OK) {
        ads_fail_cnt = 0;
        ads_ok = true;
        ESP_LOGI(TAG, "ADS1115 recovered");
    }
    return ads_ok;
}
