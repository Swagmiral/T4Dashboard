/**
 * Speed sensor HAL — PCNT hardware counting + software debounce
 *
 * Two-layer approach:
 *   1. ESP32-S3 PCNT peripheral counts raw pulses with HW glitch filter
 *      → used for odometer (total distance)
 *   2. GPIO ISR with software debounce (min period check) records
 *      timestamps of accepted pulses → used for speed calculation
 *
 * Speed is computed from the time span between first and last accepted
 * pulse within a sliding window. At very low speed (0-1 pulses in window),
 * the elapsed time since the last pulse provides an upper-bound estimate
 * that decays smoothly toward zero.
 */

#include "hal_speed.h"
#include "config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/pulse_cnt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#define SPEED_GPIO  GPIO_NUM_43

static pcnt_unit_handle_t pcnt_unit = NULL;
static pcnt_channel_handle_t pcnt_chan = NULL;

static volatile int64_t last_accepted_us = 0;
static volatile int64_t window_first_us = 0;
static volatile int64_t window_last_us = 0;
static volatile uint32_t window_count = 0;
static volatile uint32_t prev_window_count = 0;
static volatile int64_t prev_first_us = 0;
static volatile int64_t prev_last_us = 0;
static volatile int64_t window_start_us = 0;

static uint32_t min_period_us = 1500;
static uint16_t window_ms = 500;
static uint16_t stopped_ms = 2000;

static portMUX_TYPE spinlock = portMUX_INITIALIZER_UNLOCKED;

static void IRAM_ATTR speed_isr(void *arg)
{
    (void)arg;
    int64_t now = esp_timer_get_time();

    portENTER_CRITICAL_ISR(&spinlock);
    int64_t dt = now - last_accepted_us;
    if (dt >= (int64_t)min_period_us) {
        last_accepted_us = now;
        if (window_count == 0)
            window_first_us = now;
        window_last_us = now;
        window_count++;
    }
    portEXIT_CRITICAL_ISR(&spinlock);
}

void hal_speed_init(void)
{
    min_period_us = g_config.speed_min_period_us;
    if (min_period_us == 0) min_period_us = 1500;

    window_ms = g_config.speed_window_ms;
    if (window_ms == 0) window_ms = 500;

    stopped_ms = g_config.speed_stopped_ms;
    if (stopped_ms == 0) stopped_ms = 2000;

    uint16_t glitch_ns = g_config.speed_glitch_ns;
    if (glitch_ns == 0) glitch_ns = 1000;

    pcnt_unit_config_t unit_cfg = {
        .high_limit = 32767,
        .low_limit = -1,
        .flags.accum_count = true,
    };
    ESP_ERROR_CHECK(pcnt_new_unit(&unit_cfg, &pcnt_unit));

    pcnt_glitch_filter_config_t filt_cfg = {
        .max_glitch_ns = glitch_ns,
    };
    ESP_ERROR_CHECK(pcnt_unit_set_glitch_filter(pcnt_unit, &filt_cfg));

    pcnt_chan_config_t chan_cfg = {
        .edge_gpio_num = SPEED_GPIO,
        .level_gpio_num = -1,
    };
    ESP_ERROR_CHECK(pcnt_new_channel(pcnt_unit, &chan_cfg, &pcnt_chan));
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(pcnt_chan,
        PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_HOLD));

    ESP_ERROR_CHECK(pcnt_unit_enable(pcnt_unit));
    ESP_ERROR_CHECK(pcnt_unit_clear_count(pcnt_unit));
    ESP_ERROR_CHECK(pcnt_unit_start(pcnt_unit));

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

    window_start_us = esp_timer_get_time();

    ESP_LOGI("speed", "PCNT+ISR on GPIO%d (ppk=%u, glitch=%uns, debounce=%uus, window=%ums)",
             SPEED_GPIO, g_config.pulses_per_km, glitch_ns, min_period_us, window_ms);
}

uint16_t hal_speed_get_kmh(void)
{
    int64_t now = esp_timer_get_time();
    int64_t window_us = (int64_t)window_ms * 1000;
    int64_t stopped_us = (int64_t)stopped_ms * 1000;

    bool rolled = false;
    portENTER_CRITICAL(&spinlock);
    int64_t last_accepted = last_accepted_us;

    if ((now - window_start_us) >= window_us) {
        rolled = true;
        prev_window_count = window_count;
        prev_first_us = window_first_us;
        prev_last_us = window_last_us;
        window_count = 0;
        window_first_us = 0;
        window_last_us = 0;
    }

    uint32_t prev_cnt = prev_window_count;
    int64_t prev_first = prev_first_us;
    int64_t prev_last = prev_last_us;
    uint32_t live_cnt = window_count;
    int64_t live_first = window_first_us;
    int64_t live_last = window_last_us;
    portEXIT_CRITICAL(&spinlock);

    if (rolled) {
        window_start_us = now;
    }

    if ((now - last_accepted) > stopped_us) {
        return 0;
    }

    uint16_t ppk = g_config.pulses_per_km;
    if (ppk == 0) ppk = CONFIG_DEFAULT_PULSES_PER_KM;

    /* In-progress window: need 2+ accepted pulses so speed comes from real edge timing.
     * (Old path used cnt==0 && last_accepted with elapsed clamped to min_period_us, which
     *  produced ~999 km/h on the first pulse then decayed — wrong.) */
    if (live_cnt >= 2 && live_last > live_first) {
        int64_t span_us = live_last - live_first;
        uint32_t edges = live_cnt - 1;
        uint32_t speed = (uint32_t)((3600000000LL * edges) / (span_us * ppk));
        if (speed > 999) speed = 999;
        return (uint16_t)speed;
    }

    if (prev_cnt >= 2 && prev_last > prev_first) {
        int64_t span_us = prev_last - prev_first;
        uint32_t edges = prev_cnt - 1;
        uint32_t speed = (uint32_t)((3600000000LL * edges) / (span_us * ppk));
        if (speed > 999) speed = 999;
        return (uint16_t)speed;
    }

    /* Exactly one pulse in the last completed window — decay estimate (nearly stopped). */
    if (prev_cnt == 1) {
        int64_t elapsed = now - last_accepted;
        if (elapsed < (int64_t)min_period_us)
            elapsed = min_period_us;
        uint32_t max_speed = (uint32_t)(3600000000LL / (elapsed * ppk));
        if (max_speed > 999) max_speed = 999;
        return (uint16_t)max_speed;
    }

    return 0;
}

uint32_t hal_speed_get_distance_m(void)
{
    int pcnt_val = 0;
    pcnt_unit_get_count(pcnt_unit, &pcnt_val);

    uint16_t ppk = g_config.pulses_per_km;
    if (ppk == 0) ppk = CONFIG_DEFAULT_PULSES_PER_KM;
    return (uint32_t)((uint64_t)(uint32_t)pcnt_val * 1000 / ppk);
}

void hal_speed_reset_distance(void)
{
    pcnt_unit_clear_count(pcnt_unit);
}
