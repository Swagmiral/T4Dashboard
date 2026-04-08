/**
 * Speed sensor HAL — period measurement via GPIO interrupt
 *
 * Measures time between consecutive rising edges from the
 * VW T4 speed sensor (539 pulses/km) on GPIO43.
 */

#include "hal_speed.h"
#include "config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#define SPEED_GPIO          GPIO_NUM_43
#define STOPPED_TIMEOUT_US  (2 * 1000 * 1000LL)
#define MIN_PERIOD_US       1000  // reject noise shorter than 1ms
#define PBUF_MAX            9     // max configurable median depth

static volatile int64_t last_edge_us = 0;
static volatile int64_t period_buf[PBUF_MAX];
static volatile uint8_t pbuf_idx = 0;
static volatile uint8_t pbuf_cnt = 0;
static volatile uint32_t pulse_count = 0;
static uint8_t pbuf_size = 5;    // runtime filter depth from config
static int64_t init_time_us = 0;

#define STARTUP_GRACE_US  (500 * 1000LL)

static portMUX_TYPE spinlock = portMUX_INITIALIZER_UNLOCKED;

static void IRAM_ATTR speed_isr(void *arg)
{
    (void)arg;
    int64_t now = esp_timer_get_time();

    if (now - init_time_us < STARTUP_GRACE_US) return;

    portENTER_CRITICAL_ISR(&spinlock);
    int64_t dt = now - last_edge_us;
    if (dt >= MIN_PERIOD_US) {
        last_edge_us = now;
        pulse_count++;
        if (dt > STOPPED_TIMEOUT_US) {
            pbuf_cnt = 0;
            pbuf_idx = 0;
        } else {
            period_buf[pbuf_idx] = dt;
            pbuf_idx = (pbuf_idx + 1) % pbuf_size;
            if (pbuf_cnt < pbuf_size) pbuf_cnt++;
        }
    }
    portEXIT_CRITICAL_ISR(&spinlock);
}

void hal_speed_init(void)
{
    init_time_us = esp_timer_get_time();

    uint8_t fs = g_config.speed_filter_size;
    if (fs < 1) fs = 1;
    if (fs > PBUF_MAX) fs = PBUF_MAX;
    pbuf_size = fs;

    gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << SPEED_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    gpio_config(&io_cfg);
    gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    gpio_isr_handler_add(SPEED_GPIO, speed_isr, NULL);

    ESP_LOGI("speed", "Speed sensor on GPIO%d (%u pulses/km, filter=%u, accel=%u, confirm=%u)",
             SPEED_GPIO, g_config.pulses_per_km, pbuf_size,
             g_config.speed_max_accel, g_config.speed_confirm_count);
}

static int64_t median_period(int64_t *a, uint8_t n)
{
    for (uint8_t i = 1; i < n; i++) {
        int64_t key = a[i];
        int8_t j = i - 1;
        while (j >= 0 && a[j] > key) {
            a[j + 1] = a[j];
            j--;
        }
        a[j + 1] = key;
    }
    return a[n / 2];
}

uint16_t hal_speed_get_kmh(void)
{
    static uint16_t accepted = 0;
    static int64_t last_call_us = 0;
    static uint8_t confirm_cnt = 0;
    static int8_t confirm_dir = 0;  // +1 = accel, -1 = decel

    int64_t now = esp_timer_get_time();
    static int64_t buf[PBUF_MAX];
    uint8_t cnt, idx;
    int64_t last;

    portENTER_CRITICAL(&spinlock);
    cnt = pbuf_cnt;
    idx = pbuf_idx;
    for (uint8_t i = 0; i < cnt; i++) buf[i] = period_buf[i];
    last = last_edge_us;
    portEXIT_CRITICAL(&spinlock);

    if (cnt == 0 || (now - last) > STOPPED_TIMEOUT_US) {
        accepted = 0;
        confirm_cnt = 0;
        last_call_us = now;
        return 0;
    }

    int64_t p;
    if (cnt >= 3)
        p = median_period(buf, cnt);
    else
        p = buf[(idx + pbuf_size - 1) % pbuf_size];

    uint16_t ppk = g_config.pulses_per_km;
    if (ppk == 0) ppk = 539;
    uint32_t speed = (uint32_t)(3600000000LL / (p * ppk));
    if (speed > 200) speed = 0;

    uint8_t max_accel = g_config.speed_max_accel;
    uint8_t confirm_limit = g_config.speed_confirm_count;
    if (confirm_limit < 1) confirm_limit = 1;

    if (max_accel == 0 || accepted == 0) {
        accepted = (uint16_t)speed;
        confirm_cnt = 0;
    } else {
        int32_t delta = (int32_t)speed - (int32_t)accepted;
        int64_t dt_us = now - last_call_us;
        if (dt_us < 1000) dt_us = 1000;

        int32_t max_delta = (int32_t)max_accel * dt_us / 1000000;
        if (max_delta < 1) max_delta = 1;

        int32_t abs_delta = delta < 0 ? -delta : delta;

        if (abs_delta <= max_delta) {
            accepted = (uint16_t)speed;
            confirm_cnt = 0;
        } else {
            int8_t dir = (delta > 0) ? 1 : -1;
            if (dir == confirm_dir) {
                confirm_cnt++;
            } else {
                confirm_dir = dir;
                confirm_cnt = 1;
            }
            if (confirm_cnt >= confirm_limit) {
                accepted = (uint16_t)speed;
                confirm_cnt = 0;
            }
        }
    }

    last_call_us = now;
    return accepted;
}

uint32_t hal_speed_get_distance_m(void)
{
    uint32_t count;
    portENTER_CRITICAL(&spinlock);
    count = pulse_count;
    portEXIT_CRITICAL(&spinlock);

    uint16_t ppk = g_config.pulses_per_km;
    if (ppk == 0) ppk = 539;
    return (uint32_t)((uint64_t)count * 1000 / ppk);
}

void hal_speed_reset_distance(void)
{
    portENTER_CRITICAL(&spinlock);
    pulse_count = 0;
    portEXIT_CRITICAL(&spinlock);
}
