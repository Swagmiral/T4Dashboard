/**
 * I2C HAL for MCP23017 (GPIO expander) and BH1750 (light sensor)
 */

#include "hal_i2c.h"
#include "config.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"

static const char *TAG = "i2c";

// I2C configuration for Waveshare ESP32-S3-Touch-LCD-7B
#define I2C_PORT            I2C_NUM_0
#define I2C_SDA_PIN         8   // GPIO8 = SDA
#define I2C_SCL_PIN         9   // GPIO9 = SCL
// Device addresses
#define MCP23017_ADDR       0x20  // A0=A1=A2=GND
#define BH1750_ADDR         0x23  // ADDR pin LOW

// MCP23017 registers
#define MCP23017_IODIRA     0x00  // I/O direction A
#define MCP23017_IODIRB     0x01  // I/O direction B
#define MCP23017_GPPUA      0x0C  // Pull-up A
#define MCP23017_GPPUB      0x0D  // Pull-up B
#define MCP23017_GPIOA      0x12  // Port A data
#define MCP23017_GPIOB      0x13  // Port B data

// BH1750 commands
#define BH1750_POWER_ON     0x01
#define BH1750_RESET        0x07
#define BH1750_CONT_H_MODE2 0x11  // Continuous high-res mode2 (0.5 lux base)
#define BH1750_MTREG_HI(v)  (0x40 | ((v) >> 5))
#define BH1750_MTREG_LO(v)  (0x60 | ((v) & 0x1F))
#define BH1750_MTREG_VAL    254   // max sensitivity (default 69)
#define BH1750_LUX_DIVISOR   (1.2f * 2.0f * (BH1750_MTREG_VAL / 69.0f))

static i2c_master_bus_handle_t i2c_bus = NULL;
static i2c_master_dev_handle_t mcp23017_dev = NULL;
static i2c_master_dev_handle_t bh1750_dev = NULL;
static bool mcp23017_ok = false;
static bool bh1750_ok = false;
static uint8_t mcp_fail_cnt = 0;
static uint8_t bh_fail_cnt = 0;
#define I2C_FAIL_LIMIT 3

static esp_err_t mcp23017_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t data[2] = {reg, value};
    return i2c_master_transmit(mcp23017_dev, data, 2, 10);
}

static uint8_t mcp23017_read_reg(uint8_t reg)
{
    uint8_t value;
    esp_err_t ret = i2c_master_transmit_receive(mcp23017_dev, &reg, 1, &value, 1, 10);
    if (ret == ESP_OK) {
        mcp_fail_cnt = 0;
        return value;
    }
    if (++mcp_fail_cnt >= I2C_FAIL_LIMIT) {
        mcp23017_ok = false;
        ESP_LOGW(TAG, "MCP23017 lost - GPIO expander disabled");
    }
    return 0xFF;
}

/**
 * I2C bus recovery: if a slave is holding SDA low after an interrupted
 * transaction (e.g. MCU reset mid-transfer), clock out up to 9 pulses
 * on SCL to free the bus, then issue a STOP condition.
 */
static void i2c_bus_recovery(void)
{
    gpio_config_t scl_cfg = {
        .pin_bit_mask = (1ULL << I2C_SCL_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&scl_cfg);

    gpio_config_t sda_cfg = {
        .pin_bit_mask = (1ULL << I2C_SDA_PIN),
        .mode = GPIO_MODE_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&sda_cfg);
    gpio_set_level(I2C_SDA_PIN, 1);
    gpio_set_level(I2C_SCL_PIN, 1);
    esp_rom_delay_us(100);

    for (int round = 0; round < 5; round++) {
        if (gpio_get_level(I2C_SDA_PIN)) break;

        for (int i = 0; i < 18; i++) {
            gpio_set_level(I2C_SCL_PIN, 0);
            esp_rom_delay_us(10);
            gpio_set_level(I2C_SCL_PIN, 1);
            esp_rom_delay_us(10);
            if (gpio_get_level(I2C_SDA_PIN)) break;
        }

        /* STOP condition */
        gpio_set_level(I2C_SDA_PIN, 0);
        esp_rom_delay_us(10);
        gpio_set_level(I2C_SCL_PIN, 1);
        esp_rom_delay_us(10);
        gpio_set_level(I2C_SDA_PIN, 1);
        esp_rom_delay_us(100);

        if (gpio_get_level(I2C_SDA_PIN)) break;
        esp_rom_delay_us(10000);
    }

    bool sda_ok = gpio_get_level(I2C_SDA_PIN);
    bool scl_ok = gpio_get_level(I2C_SCL_PIN);

    gpio_reset_pin(I2C_SCL_PIN);
    gpio_reset_pin(I2C_SDA_PIN);

    if (sda_ok && scl_ok) ESP_LOGW(TAG, "I2C bus recovered (SDA=%d SCL=%d)", sda_ok, scl_ok);
    else ESP_LOGE(TAG, "I2C recovery FAILED (SDA=%d SCL=%d)", sda_ok, scl_ok);
}

static void i2c_create_bus(void)
{
    i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_PORT,
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t ret = i2c_new_master_bus(&bus_config, &i2c_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2C bus: %s", esp_err_to_name(ret));
        i2c_bus = NULL;
        return;
    }
    ESP_LOGW(TAG, "I2C bus created");
}

void hal_i2c_init(void)
{
    ESP_LOGW(TAG, "Initializing I2C...");

    i2c_create_bus();

    // --- MCP23017 ---
    i2c_device_config_t mcp_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = MCP23017_ADDR,
        .scl_speed_hz = g_config.i2c_scl_hz,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_bus, &mcp_config, &mcp23017_dev));

    esp_err_t ret;
    ret = mcp23017_write_reg(MCP23017_IODIRA, 0xFF);

    // If bus is in invalid state, retry: delete bus, recover, recreate
    if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "I2C bus invalid - resetting driver and retrying...");
        if (mcp23017_dev) { i2c_master_bus_rm_device(mcp23017_dev); mcp23017_dev = NULL; }
        if (i2c_bus) { i2c_del_master_bus(i2c_bus); i2c_bus = NULL; }

        esp_rom_delay_us(50000);
        i2c_create_bus();
        if (i2c_bus) {
            i2c_master_bus_add_device(i2c_bus, &mcp_config, &mcp23017_dev);
            if (mcp23017_dev) ret = mcp23017_write_reg(MCP23017_IODIRA, 0xFF);
        }
        ESP_LOGW(TAG, "I2C retry result: %s", esp_err_to_name(ret));
    }

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "MCP23017 not responding (%s) - GPIO expander disabled", esp_err_to_name(ret));
    } else {
        mcp23017_write_reg(MCP23017_IODIRB, 0xFF);
        mcp23017_write_reg(MCP23017_GPPUA, 0xFF);
        mcp23017_write_reg(MCP23017_GPPUB, 0xFF);
        mcp23017_ok = true;
        ESP_LOGI(TAG, "MCP23017 initialized");
    }

    // --- BH1750 ---
    i2c_device_config_t bh_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BH1750_ADDR,
        .scl_speed_hz = g_config.i2c_scl_hz,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_bus, &bh_config, &bh1750_dev));

    uint8_t cmd = BH1750_POWER_ON;
    ret = i2c_master_transmit(bh1750_dev, &cmd, 1, 10);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "BH1750 not responding (%s) - light sensor disabled", esp_err_to_name(ret));
        bh1750_ok = false;
    } else {
        cmd = BH1750_MTREG_HI(BH1750_MTREG_VAL);
        i2c_master_transmit(bh1750_dev, &cmd, 1, 10);
        cmd = BH1750_MTREG_LO(BH1750_MTREG_VAL);
        i2c_master_transmit(bh1750_dev, &cmd, 1, 10);
        cmd = BH1750_CONT_H_MODE2;
        i2c_master_transmit(bh1750_dev, &cmd, 1, 10);
        bh1750_ok = true;
    }

    ESP_LOGI(TAG, "I2C initialized (BH1750: %s)", bh1750_ok ? "OK" : "FAIL");
}

float hal_i2c_read_light_lux(void)
{
    if (!bh1750_ok) return 0.0f;
    uint8_t data[2] = {0};
    esp_err_t ret = i2c_master_receive(bh1750_dev, data, 2, 10);
    if (ret != ESP_OK) {
        if (++bh_fail_cnt >= I2C_FAIL_LIMIT) {
            bh1750_ok = false;
            ESP_LOGW(TAG, "BH1750 lost - light sensor disabled");
        }
        return 0.0f;
    }
    bh_fail_cnt = 0;
    uint16_t raw = (data[0] << 8) | data[1];
    return raw / BH1750_LUX_DIVISOR;
}

bool hal_i2c_read_ignition(void)
{
    if (!mcp23017_ok) return false;
    uint8_t porta = mcp23017_read_reg(MCP23017_GPIOA);
    return !(porta & (1 << 0));
}

uint16_t hal_i2c_read_inputs(void)
{
    if (!mcp23017_ok) return 0xFFFF;
    uint8_t porta = mcp23017_read_reg(MCP23017_GPIOA);
    uint8_t portb = mcp23017_read_reg(MCP23017_GPIOB);
    return (portb << 8) | porta;
}

i2c_master_bus_handle_t hal_i2c_get_bus(void)
{
    return i2c_bus;
}

bool hal_i2c_mcp_ok(void) { return mcp23017_ok; }
bool hal_i2c_bh1750_ok(void) { return bh1750_ok; }

bool hal_i2c_mcp_reprobe(void)
{
    if (mcp23017_ok || mcp23017_dev == NULL) return mcp23017_ok;
    esp_err_t ret = mcp23017_write_reg(MCP23017_IODIRA, 0xFF);
    if (ret == ESP_OK) {
        mcp23017_write_reg(MCP23017_IODIRB, 0xFF);
        mcp23017_write_reg(MCP23017_GPPUA, 0xFF);
        mcp23017_write_reg(MCP23017_GPPUB, 0xFF);
        mcp_fail_cnt = 0;
        mcp23017_ok = true;
        ESP_LOGI(TAG, "MCP23017 recovered");
    }
    return mcp23017_ok;
}

bool hal_i2c_bh1750_reprobe(void)
{
    if (bh1750_ok || bh1750_dev == NULL) return bh1750_ok;
    uint8_t cmd = BH1750_POWER_ON;
    esp_err_t ret = i2c_master_transmit(bh1750_dev, &cmd, 1, 10);
    if (ret == ESP_OK) {
        cmd = BH1750_MTREG_HI(BH1750_MTREG_VAL);
        i2c_master_transmit(bh1750_dev, &cmd, 1, 10);
        cmd = BH1750_MTREG_LO(BH1750_MTREG_VAL);
        i2c_master_transmit(bh1750_dev, &cmd, 1, 10);
        cmd = BH1750_CONT_H_MODE2;
        i2c_master_transmit(bh1750_dev, &cmd, 1, 10);
        bh_fail_cnt = 0;
        bh1750_ok = true;
        ESP_LOGI(TAG, "BH1750 recovered");
    }
    return bh1750_ok;
}
