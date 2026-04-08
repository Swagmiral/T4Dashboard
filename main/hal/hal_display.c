/**
 * Display HAL for ESP32-S3-Touch-LCD-7B (Waveshare)
 * 7" 1024x600 RGB LCD with LVGL 9.x
 */

#include "hal_display.h"
#include "ui.h"
#include "dashboard.h"
#include <string.h>
#include "esp_log.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lvgl.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"

static const char *TAG = "display";

// IO Expander (for backlight control)
#define IO_EXT_I2C_ADDR         0x24
#define IO_EXT_REG_DIRECTION    0x02
#define IO_EXT_REG_OUTPUT       0x03
#define IO_EXT_REG_PWM          0x05
#define BACKLIGHT_PWM_MAX       247

// Display configuration
#define LCD_H_RES           1024
#define LCD_V_RES           600
#define LCD_PIXEL_CLOCK_HZ  (38 * 1000 * 1000)

// RGB timing
#define LCD_HSYNC_BACK_PORCH    152
#define LCD_HSYNC_FRONT_PORCH   48
#define LCD_HSYNC_PULSE_WIDTH   162
#define LCD_VSYNC_BACK_PORCH    13
#define LCD_VSYNC_FRONT_PORCH   3
#define LCD_VSYNC_PULSE_WIDTH   45

// GPIO pins
#define PIN_LCD_DE      5
#define PIN_LCD_VSYNC   3
#define PIN_LCD_HSYNC   46
#define PIN_LCD_PCLK    7

// RGB data pins - RGB565
#define PIN_LCD_B3      14
#define PIN_LCD_B4      38
#define PIN_LCD_B5      18
#define PIN_LCD_B6      17
#define PIN_LCD_B7      10
#define PIN_LCD_G2      39
#define PIN_LCD_G3      0
#define PIN_LCD_G4      45
#define PIN_LCD_G5      48
#define PIN_LCD_G6      47
#define PIN_LCD_G7      21
#define PIN_LCD_R3      1
#define PIN_LCD_R4      2
#define PIN_LCD_R5      42
#define PIN_LCD_R6      41
#define PIN_LCD_R7      40

static esp_lcd_panel_handle_t panel_handle = NULL;
static i2c_master_dev_handle_t io_ext_dev = NULL;
static uint8_t io_ext_output_state = 0xFF;
static lv_display_t *lvgl_disp = NULL;
static SemaphoreHandle_t lvgl_mutex = NULL;

// Double framebuffers in PSRAM for tear-free rendering
static void *fb[2] = {NULL, NULL};
static const size_t fb_bytes = (size_t)LCD_H_RES * (size_t)LCD_V_RES * sizeof(uint16_t);
static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)area;
    esp_cache_msync(px_map, fb_bytes,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, LCD_H_RES, LCD_V_RES, px_map);
    lv_display_flush_ready(disp);
}

// Initialize IO Expander and turn OFF backlight
static esp_err_t io_ext_write(uint8_t reg, uint8_t val)
{
    if (io_ext_dev == NULL) return ESP_ERR_INVALID_STATE;
    uint8_t data[] = {reg, val};
    esp_err_t ret = i2c_master_transmit(io_ext_dev, data, sizeof(data), 50);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "IO Expander write reg=0x%02X failed: %s", reg, esp_err_to_name(ret));
    }
    return ret;
}

void hal_display_init_io_expander(i2c_master_bus_handle_t bus)
{
    ESP_LOGW(TAG, "Initializing IO Expander...");
    
    if (io_ext_dev == NULL) {
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = IO_EXT_I2C_ADDR,
            .scl_speed_hz = 100000,
        };
        ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev_cfg, &io_ext_dev));
    }
    
    esp_err_t ret;
    ret = io_ext_write(IO_EXT_REG_DIRECTION, 0xFF);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "IO Expander init FAILED - bus may be stuck");
        return;
    }
    io_ext_write(IO_EXT_REG_PWM, 0x00);

    io_ext_output_state = 0xFF & ~(1 << 2);
    io_ext_write(IO_EXT_REG_OUTPUT, io_ext_output_state);
    
    ESP_LOGW(TAG, "IO Expander ready, backlight OFF");
}

// Enable backlight
void hal_display_enable_backlight(i2c_master_bus_handle_t bus)
{
    (void)bus;
    io_ext_output_state |= (1 << 2);
    esp_err_t ret = io_ext_write(IO_EXT_REG_OUTPUT, io_ext_output_state);
    ESP_LOGW(TAG, "Backlight ON: %s", (ret == ESP_OK) ? "OK" : esp_err_to_name(ret));
}

// Initialize display hardware
void hal_display_init(void)
{
    ESP_LOGI(TAG, "Initializing display...");
    
    esp_lcd_rgb_panel_config_t panel_config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = LCD_PIXEL_CLOCK_HZ,
            .h_res = LCD_H_RES,
            .v_res = LCD_V_RES,
            .hsync_pulse_width = LCD_HSYNC_PULSE_WIDTH,
            .hsync_back_porch = LCD_HSYNC_BACK_PORCH,
            .hsync_front_porch = LCD_HSYNC_FRONT_PORCH,
            .vsync_pulse_width = LCD_VSYNC_PULSE_WIDTH,
            .vsync_back_porch = LCD_VSYNC_BACK_PORCH,
            .vsync_front_porch = LCD_VSYNC_FRONT_PORCH,
            .flags.pclk_active_neg = 1,
        },
        .data_width = 16,
        .bits_per_pixel = 16,
        .num_fbs = 2,
        .bounce_buffer_size_px = LCD_H_RES * 20,
        .sram_trans_align = 4,
        .psram_trans_align = 64,
        .hsync_gpio_num = PIN_LCD_HSYNC,
        .vsync_gpio_num = PIN_LCD_VSYNC,
        .de_gpio_num = PIN_LCD_DE,
        .pclk_gpio_num = PIN_LCD_PCLK,
        .disp_gpio_num = -1,
        .data_gpio_nums = {
            PIN_LCD_B3, PIN_LCD_B4, PIN_LCD_B5, PIN_LCD_B6, PIN_LCD_B7,
            PIN_LCD_G2, PIN_LCD_G3, PIN_LCD_G4, PIN_LCD_G5, PIN_LCD_G6, PIN_LCD_G7,
            PIN_LCD_R3, PIN_LCD_R4, PIN_LCD_R5, PIN_LCD_R6, PIN_LCD_R7,
        },
        .flags.fb_in_psram = 1,
    };
    
    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));

    // Get both framebuffers and clear to black
    esp_lcd_rgb_panel_get_frame_buffer(panel_handle, 2, &fb[0], &fb[1]);
    memset(fb[0], 0, fb_bytes);
    memset(fb[1], 0, fb_bytes);
    esp_cache_msync(fb[0], fb_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    esp_cache_msync(fb[1], fb_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    
    ESP_LOGI(TAG, "Display initialized");
}

// Show simple splash (before LVGL init)
void hal_display_show_splash(void)
{
    for (int b = 0; b < 2; b++) {
        if (fb[b]) {
            uint16_t *p = (uint16_t *)fb[b];
            for (int i = 0; i < LCD_H_RES * LCD_V_RES; i++) {
                p[i] = 0x000A;
            }
            esp_cache_msync(fb[b], fb_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        }
    }
    ESP_LOGI(TAG, "Splash displayed");
}

static uint32_t lvgl_tick_get_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// LVGL direct mode: single full-screen buffer = RGB panel FB in PSRAM (fast path)
void hal_display_init_ui(void)
{
    ESP_LOGI(TAG, "Initializing LVGL (direct mode, PSRAM framebuffer)...");
    
    lv_init();
    lv_tick_set_cb(lvgl_tick_get_cb);

    void *psram_pool = heap_caps_malloc(512 * 1024, MALLOC_CAP_SPIRAM);
    if (psram_pool) {
        lv_mem_add_pool(psram_pool, 512 * 1024);
        ESP_LOGI(TAG, "Added 512KB PSRAM pool to LVGL");
    } else {
        ESP_LOGE(TAG, "Failed to allocate PSRAM pool for LVGL!");
    }
    
    lvgl_disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    lv_display_set_buffers(lvgl_disp, fb[0], fb[1],
                           (uint32_t)fb_bytes,
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(lvgl_disp, lvgl_flush_cb);
    lv_display_set_color_format(lvgl_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_antialiasing(lvgl_disp, true);

    lvgl_mutex = xSemaphoreCreateMutex();
    
    ui_init();
    
    ESP_LOGI(TAG, "LVGL initialized (direct mode)");
}

// LVGL tick handler - call from main loop
void hal_display_tick(void)
{
    if (lvgl_mutex && xSemaphoreTake(lvgl_mutex, pdMS_TO_TICKS(10))) {
        lv_timer_handler();
        ui_tick();
        xSemaphoreGive(lvgl_mutex);
    }
}

// Lock LVGL for thread-safe UI updates
bool hal_display_lock(int timeout_ms)
{
    if (lvgl_mutex == NULL) return false;
    return xSemaphoreTake(lvgl_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

// Unlock LVGL
void hal_display_unlock(void)
{
    if (lvgl_mutex) {
        xSemaphoreGive(lvgl_mutex);
    }
}

void hal_display_set_voltage_valid(bool valid, float volts)
{
    if (lvgl_mutex == NULL) {
        return;
    }
    if (xSemaphoreTake(lvgl_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        dashboard_feed_voltage(valid, volts);
        xSemaphoreGive(lvgl_mutex);
    }
}

void hal_display_set_brightness(uint8_t level)
{
    uint8_t pwm_val = (uint8_t)(((uint16_t)(255 - level) * BACKLIGHT_PWM_MAX) / 255);
    io_ext_write(IO_EXT_REG_PWM, pwm_val);
}

void hal_display_set_odometer(uint32_t km)
{
    if (lvgl_mutex == NULL) {
        return;
    }
    if (xSemaphoreTake(lvgl_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        dashboard_set_odometer(km);
        xSemaphoreGive(lvgl_mutex);
    }
}
