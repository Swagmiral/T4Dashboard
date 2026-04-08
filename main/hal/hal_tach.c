/**
 * Tachometer HAL — period measurement via GPIO interrupt
 *
 * Measures time between consecutive rising edges from the
 * engine tachometer signal on GPIO44 (UART2 RX).
 * Signal is conditioned by 74HC14 Schmitt trigger.
 */

#include "hal_tach.h"
#include "config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#define TACH_GPIO           GPIO_NUM_44
#define STOPPED_TIMEOUT_US  (2 * 1000 * 1000LL)
#define MIN_PERIOD_US       500   // reject noise shorter than 0.5ms (~120 000 RPM)

static volatile int64_t last_edge_us = 0;
static volatile int64_t period_us = 0;

static portMUX_TYPE tach_spinlock = portMUX_INITIALIZER_UNLOCKED;

static void IRAM_ATTR tach_isr(void *arg)
{
    (void)arg;
    int64_t now = esp_timer_get_time();

    portENTER_CRITICAL_ISR(&tach_spinlock);
    int64_t dt = now - last_edge_us;
    if (dt >= MIN_PERIOD_US) {
        period_us = dt;
        last_edge_us = now;
    }
    portEXIT_CRITICAL_ISR(&tach_spinlock);
}

void hal_tach_init(void)
{
    gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << TACH_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    gpio_config(&io_cfg);
    gpio_isr_handler_add(TACH_GPIO, tach_isr, NULL);

    ESP_LOGI("tach", "Tachometer on GPIO%d (%u pulse/rev)",
             TACH_GPIO, g_config.tach_pulses_per_rev);
}

uint16_t hal_tach_get_rpm(void)
{
    int64_t now = esp_timer_get_time();
    int64_t p, last;

    portENTER_CRITICAL(&tach_spinlock);
    p = period_us;
    last = last_edge_us;
    portEXIT_CRITICAL(&tach_spinlock);

    if (p == 0 || (now - last) > STOPPED_TIMEOUT_US) {
        return 0;
    }

    uint8_t ppr = g_config.tach_pulses_per_rev;
    if (ppr == 0) ppr = 1;

    // rpm = 60 000 000 / (period_us * pulses_per_rev)
    uint32_t rpm = (uint32_t)(60000000LL / (p * ppr));

    uint16_t max_rpm = g_config.tach_max_rpm ? g_config.tach_max_rpm : 6000;
    if (rpm > max_rpm) return 0;
    return (uint16_t)rpm;
}
