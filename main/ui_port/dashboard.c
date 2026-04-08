#include "dashboard.h"
#include "config.h"
#include "hal_wifi.h"
#include "hal_i2c.h"
#include "hal_adc.h"
#include "screens.h"
#include "ui.h"
#include <string.h>
#include <stdio.h>

// Shorthand color accessors from config
#define C_NORM   (g_config.color_normal)
#define C_RED    (g_config.color_red)
#define C_GRN    (g_config.color_green)
#define C_YEL    (g_config.color_yellow)
#define C_COLD   (g_config.color_cold)
#define C_HB     (g_config.color_high_beam)
#define C_GP     (g_config.color_glow_plug)
#define C_DIM    (g_config.color_dim)
#define C_DIS    (g_config.color_disabled)
#define C_TOK    (g_config.color_temp_ok)
#define C_FOG    (g_config.color_fog)

// Warning types
typedef enum {
    WARNING_LOW_BATTERY = 0,
    WARNING_OVERVOLTAGE,
    WARNING_OVERHEATING,
    WARNING_BRAKE,
    WARNING_OIL_PRESSURE,
    WARNING_COUNT
} warning_type_t;

static const char* warning_texts[WARNING_COUNT] = {
    "Low Battery!",
    "Overvoltage!",
    "Overheating!",
    "Brake!",
    "Oil Pressure!"
};

// Warning queue system
static bool warnings_active[WARNING_COUNT] = {false, false, false, false, false};
static uint8_t current_warning_index = 0;
static uint16_t current_speed = 0;
static uint8_t current_tach_level = 0;
static uint8_t current_temp = 50;   // For HOT blink logic
static uint8_t current_fuel = 50;   // For fuel warning logic
static bool fuel_warning_on = false;
static bool glow_plug_on = false;
static bool warning_visible = false;
static bool showing_static_brake = false;
static uint16_t sim_flags_dash = 0;

static bool dis_blinker_l = false, dis_blinker_r = false;
static bool dis_high_beam = false, dis_fog_light = false;
static bool dis_oil = false, dis_brake = false, dis_glow = false;

// ============================================================================
// FUEL SMOOTHING SYSTEM
// ============================================================================
#define FUEL_BUFFER_MAX 30
#define FUEL_SAMPLE_FIRST_RUN_MS 25

typedef enum {
    FUEL_STATE_FIRST_RUN,       // No display value yet
    FUEL_STATE_MOVING,          // Vehicle moving (speed > 0)
    FUEL_STATE_STOPPING_DELAY,  // Just stopped, waiting 30 sec
    FUEL_STATE_STOPPED          // Stopped, fast sampling
} fuel_state_t;

static struct {
    uint8_t buffer[FUEL_BUFFER_MAX];
    uint8_t buffer_count;
    uint8_t displayed_value;
    bool has_displayed_value;
    fuel_state_t state;
    uint32_t last_sample_time;
    uint32_t state_change_time;
    uint32_t emergency_start_time;
    bool emergency_active;
    int8_t hysteresis_direction;    // -1 = falling, 0 = stable, +1 = rising
    uint8_t hysteresis_count;       // Cycles in same direction
    uint8_t hysteresis_value;       // Pending value to apply
} fuel_system = {
    .buffer_count = 0,
    .displayed_value = 0,
    .has_displayed_value = false,
    .state = FUEL_STATE_FIRST_RUN,
    .last_sample_time = 0,
    .state_change_time = 0,
    .emergency_start_time = 0,
    .emergency_active = false,
    .hysteresis_direction = 0,
    .hysteresis_count = 0,
    .hysteresis_value = 0
};

// ============================================================================
// INPUT DEBOUNCE SYSTEM (Hybrid: interrupt-triggered + sample confirmation)
// ============================================================================
#define DEBOUNCE_SAMPLES 3
#define DEBOUNCE_INTERVAL_MS 25

typedef enum {
    INPUT_BRAKE = 0,
    INPUT_OIL,
    INPUT_HIGH_BEAM,
    INPUT_FOG_LIGHT,
    INPUT_GLOW_PLUG,
    INPUT_COUNT
} debounce_input_type_t;

typedef struct {
    bool raw_state;             // Last raw input state
    bool target_state;          // State being confirmed
    bool confirmed_state;       // Current confirmed state
    uint8_t confirm_count;      // Confirmation counter
    bool confirming;            // Confirmation in progress
} debounce_input_t;

static debounce_input_t debounce_inputs[INPUT_COUNT] = {0};

// Callback type for applying confirmed state
typedef void (*input_apply_fn)(bool state);

// Apply functions for each input
static void apply_brake(bool on) { dashboard_set_brake_warning(on); }
static void apply_oil(bool on) { dashboard_set_oil_warning(on); }
static void apply_high_beam(bool on) { dashboard_set_high_beam(on); }
static void apply_fog_light(bool on) { dashboard_set_fog_light(on); }
static void apply_glow_plug(bool on) { dashboard_set_glow_plug(on); }

static const input_apply_fn input_apply_fns[INPUT_COUNT] = {
    apply_brake,
    apply_oil,
    apply_high_beam,
    apply_fog_light,
    apply_glow_plug
};

// Called from HAL when raw GPIO changes (reserved; debounce_process polls today)
__attribute__((unused)) static void debounce_input_changed(debounce_input_type_t type, bool new_state) {
    debounce_input_t *inp = &debounce_inputs[type];
    inp->raw_state = new_state;
    
    if (new_state != inp->confirmed_state && !inp->confirming) {
        // Start confirmation
        inp->target_state = new_state;
        inp->confirm_count = 1;
        inp->confirming = true;
    }
}

// Process debounce - called every 25ms from timer
static void debounce_process(void) {
    for (int i = 0; i < INPUT_COUNT; i++) {
        debounce_input_t *inp = &debounce_inputs[i];
        if (!inp->confirming) continue;
        
        if (inp->raw_state == inp->target_state) {
            inp->confirm_count++;
            if (inp->confirm_count >= DEBOUNCE_SAMPLES) {
                // Confirmed!
                inp->confirmed_state = inp->target_state;
                inp->confirming = false;
                input_apply_fns[i](inp->confirmed_state);
            }
        } else {
            // State changed during confirmation - restart
            inp->confirming = false;
            inp->confirm_count = 0;
        }
    }
}

// ============================================================================
// VOLTAGE SMOOTHING SYSTEM (exponential moving average)
// ============================================================================
static struct {
    float ema;
    uint8_t samples;
    bool primed;
} voltage_system = {0};

void dashboard_feed_voltage(bool valid, float raw_voltage) {
    if (!valid) {
        voltage_system.primed = false;
        voltage_system.samples = 0;
        dashboard_set_voltage_valid(false, 0.0f);
        return;
    }

    if (!voltage_system.primed) {
        voltage_system.ema = raw_voltage;
        voltage_system.primed = true;
        voltage_system.samples = 1;
    } else {
        voltage_system.ema += g_config.voltage_ema_alpha * (raw_voltage - voltage_system.ema);
        if (voltage_system.samples < g_config.voltage_settle_count) {
            voltage_system.samples++;
        }
    }

    if (voltage_system.samples < g_config.voltage_settle_count) {
        return;
    }

    dashboard_set_voltage_valid(true, voltage_system.ema);
}

// ============================================================================
// TEMPERATURE SMOOTHING SYSTEM
// ============================================================================
#define TEMP_BUFFER_SIZE 8

static struct {
    uint8_t buffer[TEMP_BUFFER_SIZE];
    uint8_t index;
    uint8_t count;
} temp_smooth_system = {0};

static void temp_system_process(uint8_t raw_temp) {
    // Add to circular buffer
    temp_smooth_system.buffer[temp_smooth_system.index] = raw_temp;
    temp_smooth_system.index = (temp_smooth_system.index + 1) % TEMP_BUFFER_SIZE;
    if (temp_smooth_system.count < TEMP_BUFFER_SIZE) {
        temp_smooth_system.count++;
    }
    
    // Calculate average
    uint16_t sum = 0;
    for (int i = 0; i < temp_smooth_system.count; i++) {
        sum += temp_smooth_system.buffer[i];
    }
    uint8_t avg = sum / temp_smooth_system.count;
    
    // Apply smoothed value
    dashboard_set_temp_level(avg);
}

// Forward declarations
static void fuel_system_process(uint8_t raw_fuel, uint16_t speed);
static int8_t get_next_warning(int8_t from);
static void update_glow_plug_display(void);
void warning_set(warning_type_t type, bool active);

// Timer for input polling
static lv_timer_t *dashboard_timer = NULL;

// Shared blink timer for all warnings (HOT, warning messages, etc.)
static bool blink_on = true;  // true = on, false = off

void dashboard_init(void) {
    // Hide warnings by default; opaque bg lets LVGL skip layers behind it
    if (objects.warning) {
        lv_obj_add_flag(objects.warning, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_opa(objects.warning, 255, 0);
    }
    if (objects.notification) {
        lv_obj_add_flag(objects.notification, LV_OBJ_FLAG_HIDDEN);
    }
    if (objects.glowplug_container) {
        lv_obj_add_flag(objects.glowplug_container, LV_OBJ_FLAG_HIDDEN);
    }
    
    // Initial values (set directly, not via dashboard_set_* which has early-return guards)
    if (objects.speedo && lv_obj_is_valid(objects.speedo))
        lv_arc_set_value(objects.speedo, 0);
    if (objects.speed && lv_obj_is_valid(objects.speed))
        lv_label_set_text(objects.speed, "0");

    // Reset fuel bar and mover to 0
    if (objects.fuel_bar)
        lv_bar_set_value(objects.fuel_bar, 0, LV_ANIM_OFF);
    if (objects.fuel_mover)
        lv_obj_set_y(objects.fuel_mover, 90);  // y_offset at value 0: (50-0)*180/100

    // Reset temp bar and mover to 0
    if (objects.temperature)
        lv_bar_set_value(objects.temperature, 0, LV_ANIM_OFF);
    if (objects.temp_mover)
        lv_obj_set_y(objects.temp_mover, 90);

    // Show placeholder text until real data arrives
    if (objects.fuel_value) {
        lv_label_set_text(objects.fuel_value, "- - -");
        lv_obj_set_style_text_color(objects.fuel_value, lv_color_hex(0x808080), 0);
    }
    if (objects.temp_status) {
        lv_label_set_text(objects.temp_status, "- - -");
        lv_obj_set_style_text_color(objects.temp_status, lv_color_hex(0x808080), 0);
    }
    dashboard_set_voltage_valid(false, 0.0f);
    dashboard_set_odometer(0);
    
    // All indicators off
    dashboard_set_turn_left(false);
    dashboard_set_turn_right(false);
    dashboard_set_high_beam(false);
    dashboard_set_fog_light(false);
    dashboard_set_oil_warning(false);
    dashboard_set_brake_warning(false);
    dashboard_set_glow_plug(false);
    dashboard_set_battery_warning(false);
    
    // Dim all tach LEDs
    {
        lv_obj_t *leds[] = { objects.led1, objects.led2, objects.led3,
                             objects.led4, objects.led5, objects.led6 };
        for (int i = 0; i < 6; i++) {
            if (leds[i] && lv_obj_is_valid(leds[i])) {
                lv_obj_set_style_image_recolor(leds[i], lv_color_hex(C_DIM), 0);
                lv_obj_set_style_image_opa(leds[i], 255, 0);
            }
        }
    }

    // Override EEZ-hardcoded colors with config values
    if (objects.fuel_bar)
        lv_obj_set_style_bg_color(objects.fuel_bar, lv_color_hex(g_config.color_bar_bg), LV_PART_MAIN);
    if (objects.temperature)
        lv_obj_set_style_bg_color(objects.temperature, lv_color_hex(g_config.color_bar_bg), LV_PART_MAIN);
    if (objects.speedogrid)
        lv_obj_set_style_image_recolor(objects.speedogrid, lv_color_hex(g_config.color_speedo_bg), 0);
    if (objects.speedo)
        lv_obj_set_style_arc_color(objects.speedo, lv_color_hex(g_config.color_speedo_ind), LV_PART_INDICATOR);

    // Mark disabled indicators (pin=255) with disabled color
    dis_blinker_l = (g_config.pin_blinker_l == 255);
    dis_blinker_r = (g_config.pin_blinker_r == 255);
    dis_high_beam = (g_config.pin_high_beam == 255);
    dis_fog_light = (g_config.pin_fog_light == 255);
    dis_oil       = (g_config.pin_oil == 255);
    dis_brake     = (g_config.pin_brake == 255);
    dis_glow      = (g_config.pin_glow_plug == 255);

    lv_color_t dc = lv_color_hex(C_DIS);
    if (dis_blinker_l && objects.blinker_l)   lv_obj_set_style_image_recolor(objects.blinker_l, dc, 0);
    if (dis_blinker_r && objects.blinker_r)   lv_obj_set_style_image_recolor(objects.blinker_r, dc, 0);
    if (dis_high_beam && objects.highbeam_icon) lv_obj_set_style_image_recolor(objects.highbeam_icon, dc, 0);
    if (dis_fog_light && objects.foglight_icon) lv_obj_set_style_image_recolor(objects.foglight_icon, dc, 0);
    if (dis_oil && objects.oil_icon)           lv_obj_set_style_image_recolor(objects.oil_icon, dc, 0);
    if (dis_brake && objects.brake_icon)       lv_obj_set_style_image_recolor(objects.brake_icon, dc, 0);
    if (dis_glow && objects.glowplug_container) lv_obj_set_style_image_recolor(objects.glowplug_container, dc, 0);
}

void dashboard_set_speed(uint16_t kmh) {
    if (kmh > 999) kmh = 999;
    uint16_t prev_speed = current_speed;
    current_speed = kmh;

    // Arc: always update EMA so it keeps smoothing toward target
    if (objects.speedo && lv_obj_is_valid(objects.speedo)) {
        static float arc_ema = 0.0f;
        uint16_t smax = g_config.speed_max_kmh ? g_config.speed_max_kmh : 180;
        float target = (kmh >= smax) ? 100.0f : (kmh * 100.0f / smax);
        float alpha = g_config.speed_arc_smooth;
        if (alpha <= 0.0f || alpha > 1.0f) alpha = 1.0f;
        if (prev_speed == 0xFFFF || (prev_speed == 0 && kmh > 0))
            arc_ema = target;
        else
            arc_ema += alpha * (target - arc_ema);
        if (arc_ema < 0.5f) arc_ema = 0.0f;
        uint8_t pct = (uint8_t)(arc_ema + 0.5f);
        lv_arc_set_value(objects.speedo, pct);
    }

    if (glow_plug_on && kmh != prev_speed && ((prev_speed == 0) != (kmh == 0))) {
        update_glow_plug_display();
    }

    // Label: throttled with hysteresis
    static uint32_t last_text_update = 0;
    static uint16_t displayed_speed = 0;
    uint32_t now = lv_tick_get();
    uint16_t interval = (kmh < g_config.speed_slow_threshold)
                        ? g_config.speed_slow_interval_ms
                        : g_config.speed_text_interval_ms;
    if (last_text_update != 0 && now - last_text_update < interval) return;

    int diff = (int)kmh - (int)displayed_speed;
    if (diff < 0) diff = -diff;
    if (diff < g_config.speed_text_hyst && displayed_speed != 0 && kmh != 0) return;

    last_text_update = now;
    displayed_speed = kmh;
    if (objects.speed && lv_obj_is_valid(objects.speed)) {
        lv_label_set_text_fmt(objects.speed, "%d", kmh);
    }
}

void dashboard_set_rpm(uint16_t rpm) {
    uint16_t max_rpm = g_config.tach_max_rpm ? g_config.tach_max_rpm : 6000;
    if (rpm > max_rpm) rpm = max_rpm;

    uint16_t step = max_rpm / 6;
    uint16_t redline = max_rpm - step / 2;

    uint8_t level;
    if (step == 0 || rpm < step) level = 0;
    else if (rpm >= redline) level = 6;
    else {
        level = rpm / step;
        if (level > 5) level = 5;
    }

    dashboard_set_tach_level(level);
}

void dashboard_set_tach_level(uint8_t level) {
    if (level > 6) level = 6;
    if (level == current_tach_level) return;
    current_tach_level = level;
    
    lv_obj_t *leds[] = {
        objects.led1, objects.led2, objects.led3,
        objects.led4, objects.led5, objects.led6
    };
    
    for (int i = 0; i < 6; i++) {
        if (!leds[i] || !lv_obj_is_valid(leds[i])) continue;
        uint32_t color;
        if (i < level) {
            if (i == 5) color = C_RED;
            else if (i == 4) color = C_YEL;
            else color = C_GRN;
        } else {
            color = C_DIM;
        }
        lv_obj_set_style_image_recolor(leds[i], lv_color_hex(color), 0);
    }
}

// Fuel display animation state
static int16_t fuel_display_current = -1;  // Currently displayed value
static bool fuel_anim_running = false;

// Animation callback - updates bar, text, position
static void fuel_anim_cb(void *var, int32_t value) {
    (void)var;
    
    uint8_t percent = (uint8_t)value;
    fuel_display_current = value;
    
    if (!objects.fuel_bar) return;
    
    lv_bar_set_value(objects.fuel_bar, percent, LV_ANIM_OFF);
    
    if (objects.fuel_value)
        lv_label_set_text_fmt(objects.fuel_value, "%d%%", percent);
    
    if (!fuel_warning_on && current_fuel < g_config.fuel_warn_on_pct)
        fuel_warning_on = true;
    else if (fuel_warning_on && current_fuel > g_config.fuel_warn_off_pct)
        fuel_warning_on = false;

    uint32_t color = (fuel_warning_on || (sim_flags_dash & SIM_LOW_FUEL)) ? C_YEL : C_NORM;
    if (objects.fuel_value)
        lv_obj_set_style_text_color(objects.fuel_value, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(objects.fuel_bar, lv_color_hex(color), LV_PART_INDICATOR);
    
    if (objects.fuel_icon)
        lv_obj_set_style_image_recolor(objects.fuel_icon, lv_color_hex(color), 0);
    if (objects.fuel_indicator)
        lv_obj_set_style_line_color(objects.fuel_indicator, lv_color_hex(color), 0);
    
    if (objects.fuel_mover) {
        int32_t y_offset = (int32_t)(50 - percent) * 180 / 100;
        lv_obj_set_y(objects.fuel_mover, y_offset);
    }
}

static void fuel_anim_completed_cb(lv_anim_t *a) {
    (void)a;
    fuel_anim_running = false;
}

void dashboard_set_fuel(uint8_t percent) {
    if (percent > 100) percent = 100;
    current_fuel = percent;  // Store for fuel warning logic
    
    // First time - animate from 0
    if (fuel_display_current < 0) {
        fuel_display_current = 0;
        fuel_anim_cb(NULL, 0);
        // Don't return - let animation start below
    }
    
    // Same value - skip
    if (fuel_display_current == percent) return;
    
    // If animation running - skip (don't interrupt)
    if (fuel_anim_running) return;
    
    // Start animation
    fuel_anim_running = true;
    
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, NULL);
    lv_anim_set_values(&a, fuel_display_current, percent);
    lv_anim_set_time(&a, 1000);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, fuel_anim_cb);
    lv_anim_set_completed_cb(&a, fuel_anim_completed_cb);
    lv_anim_start(&a);
}

void dashboard_set_temp_ok(bool ok) {
    if (!objects.temp_status) return;
    if (ok) {
        lv_label_set_text(objects.temp_status, "OK");
        lv_obj_set_style_text_color(objects.temp_status, lv_color_hex(C_TOK), 0);
    }
}

void dashboard_set_temp(int8_t celsius) {
    if (!objects.temp_status) return;
    lv_label_set_text_fmt(objects.temp_status, "%d°", celsius);
    if (celsius > 100) {
        lv_obj_set_style_text_color(objects.temp_status, lv_color_hex(C_RED), 0);
    } else {
        lv_obj_set_style_text_color(objects.temp_status, lv_color_hex(C_TOK), 0);
    }
}

// Temperature display animation state
static int16_t temp_display_current = -1;  // Currently displayed value (-1 = not initialized)
static bool temp_anim_running = false;
static uint8_t temp_state = 1;  // 0=COLD, 1=OK, 2=HOT (start at OK)

// Animation callback - updates bar, label position, text and color
static void temp_anim_cb(void *var, int32_t value) {
    (void)var;
    temp_display_current = value;
    
    if (!objects.temperature) return;
    
    lv_bar_set_value(objects.temperature, value, LV_ANIM_OFF);
    
    uint32_t color;
    if (value < 33) {
        if (objects.temp_status) lv_label_set_text(objects.temp_status, "COLD");
        color = C_COLD;
    } else if (value <= 67) {
        if (objects.temp_status) lv_label_set_text(objects.temp_status, "OK");
        color = C_TOK;
    } else {
        if (objects.temp_status) lv_label_set_text(objects.temp_status, "HOT");
        color = C_RED;
    }
    if (objects.temp_status)
        lv_obj_set_style_text_color(objects.temp_status, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(objects.temperature, lv_color_hex(color), LV_PART_INDICATOR);
    
    if (objects.temp_icon)
        lv_obj_set_style_image_recolor(objects.temp_icon, lv_color_hex(value > 67 ? C_RED : C_TOK), 0);
    if (objects.temp_indicator_line)
        lv_obj_set_style_line_color(objects.temp_indicator_line, lv_color_hex(color), 0);
    
    if (objects.temp_mover) {
        int32_t y_offset = (int32_t)(50 - value) * 180 / 100;
        lv_obj_set_y(objects.temp_mover, y_offset);
    }
}

static void temp_anim_completed_cb(lv_anim_t *a) {
    (void)a;
    temp_anim_running = false;
}

// Set temperature level (0-100%) with COLD/OK/HOT text and moving label
void dashboard_set_temp_level(uint8_t percent) {
    if (percent > 100) percent = 100;
    current_temp = percent;  // Store for HOT blink logic
    
    if (!objects.temp_status || !objects.temperature) return;
    if (!lv_obj_is_valid(objects.temp_status) || !lv_obj_is_valid(objects.temperature)) return;
    
    if (temp_state == 1) {
        if (percent <= g_config.temp_cold_on_pct)  temp_state = 0;
        else if (percent >= g_config.temp_hot_on_pct) temp_state = 2;
    } else if (temp_state == 0) {
        if (percent >= g_config.temp_cold_off_pct) temp_state = 1;
    } else {
        if (percent <= g_config.temp_hot_off_pct)  temp_state = 1;
    }
    
    int16_t target = percent;
    
    // First time - start from 0
    if (temp_display_current < 0) {
        temp_display_current = 0;
        temp_anim_cb(NULL, 0);
    }
    
    // Apply display hysteresis to prevent jitter
    int16_t diff = target - temp_display_current;
    if (diff < 0) diff = -diff;
    if (diff < g_config.temp_display_hyst && temp_display_current > 0) return;

    // Skip if same target or animation running
    if (temp_display_current == target || temp_anim_running) return;
    
    // Start animation
    temp_anim_running = true;
    
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, NULL);
    lv_anim_set_values(&a, temp_display_current, target);
    lv_anim_set_time(&a, 2000);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_set_exec_cb(&a, temp_anim_cb);
    lv_anim_set_completed_cb(&a, temp_anim_completed_cb);
    lv_anim_start(&a);
}

void dashboard_set_voltage_valid(bool valid, float volts) {
    static uint32_t low_voltage_start = 0;
    static uint32_t high_voltage_start = 0;
    static uint32_t low_recovery_start = 0;
    static uint32_t high_recovery_start = 0;
    static bool low_warning_active = false;
    static bool high_warning_active = false;
    static uint8_t color_state = 0;  // 0=white, 1=red, 2=yellow

    if (!objects.voltage) return;
    if (!lv_obj_is_valid(objects.voltage)) return;

    if (!valid) {
        lv_label_set_text(objects.voltage, "- - -");
        lv_obj_set_style_text_color(objects.voltage, lv_color_hex(0x808080), 0);
        if (objects.battery_icon && lv_obj_is_valid(objects.battery_icon)) {
            lv_obj_set_style_image_recolor(objects.battery_icon, lv_color_hex(C_NORM), 0);
            lv_obj_set_style_image_opa(objects.battery_icon, 255, 0);
        }
        color_state = 0;
        low_voltage_start = 0;
        high_voltage_start = 0;
        low_recovery_start = 0;
        high_recovery_start = 0;
        if (low_warning_active) {
            warning_set(WARNING_LOW_BATTERY, false);
            low_warning_active = false;
        }
        if (high_warning_active) {
            warning_set(WARNING_OVERVOLTAGE, false);
            high_warning_active = false;
        }
        return;
    }

    static int displayed_v10 = -1;

    int v10;
    if (displayed_v10 < 0) {
        v10 = (int)(volts * 10.0f + 0.5f);
    } else {
        float center = displayed_v10 * 0.1f;
        if (volts > center + g_config.voltage_display_hyst ||
            volts < center - g_config.voltage_display_hyst) {
            v10 = (int)(volts * 10.0f + 0.5f);
        } else {
            v10 = displayed_v10;
        }
    }
    if (v10 < 0) v10 = 0;

    if (v10 != displayed_v10) {
        displayed_v10 = v10;
        lv_label_set_text_fmt(objects.voltage, "%d.%dv", displayed_v10 / 10, displayed_v10 % 10);
    }

    switch (color_state) {
    case 0:
        if (volts < g_config.color_red_enter_v)       color_state = 1;
        else if (volts > g_config.color_yellow_enter_v) color_state = 2;
        break;
    case 1:
        if (volts > g_config.color_yellow_enter_v)     color_state = 2;
        else if (volts > g_config.color_red_exit_v)    color_state = 0;
        break;
    case 2:
        if (volts < g_config.color_red_enter_v)        color_state = 1;
        else if (volts < g_config.color_yellow_exit_v) color_state = 0;
        break;
    }

    static uint8_t prev_color_state = 0xFF;
    if (color_state != prev_color_state) {
        prev_color_state = color_state;

        uint32_t icon_color;
        if (color_state == 1) {
            icon_color = C_RED;
        } else if (color_state == 2) {
            icon_color = C_YEL;
        } else {
            icon_color = C_NORM;
        }

        lv_obj_set_style_text_color(objects.voltage, lv_color_hex(icon_color), 0);

        if (objects.battery_icon && lv_obj_is_valid(objects.battery_icon)) {
            lv_obj_set_style_image_recolor(objects.battery_icon, lv_color_hex(icon_color), 0);
            lv_obj_set_style_image_opa(objects.battery_icon, 255, 0);
        }
    }

    uint32_t now = lv_tick_get();

    if (volts < g_config.warn_low_enter_v) {
        low_recovery_start = 0;
        high_voltage_start = 0;
        high_recovery_start = 0;
        if (high_warning_active) {
            warning_set(WARNING_OVERVOLTAGE, false);
            high_warning_active = false;
        }
        if (low_voltage_start == 0) {
            low_voltage_start = now;
        } else if (!low_warning_active &&
                   (now - low_voltage_start >= g_config.warn_low_enter_ms)) {
            warning_set(WARNING_LOW_BATTERY, true);
            low_warning_active = true;
        }
    } else if (low_warning_active) {
        low_voltage_start = 0;
        if (volts > g_config.warn_low_exit_v) {
            if (low_recovery_start == 0) {
                low_recovery_start = now;
            } else if (now - low_recovery_start >= g_config.warn_low_exit_ms) {
                warning_set(WARNING_LOW_BATTERY, false);
                low_warning_active = false;
                low_recovery_start = 0;
            }
        } else {
            low_recovery_start = 0;
        }
    } else {
        low_voltage_start = 0;
        low_recovery_start = 0;
    }

    if (volts > g_config.warn_high_enter_v) {
        high_recovery_start = 0;
        low_voltage_start = 0;
        low_recovery_start = 0;
        if (low_warning_active) {
            warning_set(WARNING_LOW_BATTERY, false);
            low_warning_active = false;
        }
        if (high_voltage_start == 0) {
            high_voltage_start = now;
        } else if (!high_warning_active &&
                   (now - high_voltage_start >= g_config.warn_high_enter_ms)) {
            warning_set(WARNING_OVERVOLTAGE, true);
            high_warning_active = true;
        }
    } else if (high_warning_active) {
        high_voltage_start = 0;
        if (volts < g_config.warn_high_exit_v) {
            if (high_recovery_start == 0) {
                high_recovery_start = now;
            } else if (now - high_recovery_start >= g_config.warn_high_exit_ms) {
                warning_set(WARNING_OVERVOLTAGE, false);
                high_warning_active = false;
                high_recovery_start = 0;
            }
        } else {
            high_recovery_start = 0;
        }
    } else {
        high_voltage_start = 0;
        high_recovery_start = 0;
    }
}

void dashboard_set_odometer(uint32_t km) {
    if (km > 999999) km = 999999;
    if (!objects.odometer) return;
    lv_label_set_text_fmt(objects.odometer, "%lu KM", km);
}

// Helper function for indicator icons
static void set_indicator_icon(lv_obj_t *obj, bool on, uint32_t color_on) {
    if (on) {
        lv_obj_set_style_image_recolor(obj, lv_color_hex(color_on), 0);
        lv_obj_set_style_image_opa(obj, 255, 0);
    } else {
        lv_obj_set_style_image_recolor(obj, lv_color_hex(C_DIM), 0);
        lv_obj_set_style_image_opa(obj, 255, 0);
    }
}

void dashboard_set_turn_left(bool on) {
    if (dis_blinker_l) return;
    if (!objects.blinker_l || !lv_obj_is_valid(objects.blinker_l)) return;
    set_indicator_icon(objects.blinker_l, on, C_GRN);
    if (objects.blinker_l_gradient && lv_obj_is_valid(objects.blinker_l_gradient)) {
        if (on) lv_obj_clear_flag(objects.blinker_l_gradient, LV_OBJ_FLAG_HIDDEN);
        else    lv_obj_add_flag(objects.blinker_l_gradient, LV_OBJ_FLAG_HIDDEN);
    }
}

void dashboard_set_turn_right(bool on) {
    if (dis_blinker_r) return;
    if (!objects.blinker_r || !lv_obj_is_valid(objects.blinker_r)) return;
    set_indicator_icon(objects.blinker_r, on, C_GRN);
    if (objects.blinker_r_gradient && lv_obj_is_valid(objects.blinker_r_gradient)) {
        if (on) lv_obj_clear_flag(objects.blinker_r_gradient, LV_OBJ_FLAG_HIDDEN);
        else    lv_obj_add_flag(objects.blinker_r_gradient, LV_OBJ_FLAG_HIDDEN);
    }
}

void dashboard_set_high_beam(bool on) {
    if (dis_high_beam) return;
    if (!objects.highbeam_icon || !lv_obj_is_valid(objects.highbeam_icon)) return;
    set_indicator_icon(objects.highbeam_icon, on, C_HB);
}

void dashboard_set_fog_light(bool on) {
    if (dis_fog_light) return;
    if (!objects.foglight_icon || !lv_obj_is_valid(objects.foglight_icon)) return;
    set_indicator_icon(objects.foglight_icon, on, C_FOG);
}

void dashboard_set_oil_warning(bool on) {
    if (dis_oil) return;
    if (!objects.oil_icon || !lv_obj_is_valid(objects.oil_icon)) return;
    set_indicator_icon(objects.oil_icon, on, C_RED);
    warning_set(WARNING_OIL_PRESSURE, on);
}

void dashboard_set_brake_warning(bool on) {
    if (dis_brake) return;
    if (!objects.brake_icon || !lv_obj_is_valid(objects.brake_icon)) return;
    set_indicator_icon(objects.brake_icon, on, C_RED);
    warning_set(WARNING_BRAKE, on);
}

static void update_glow_plug_display(void) {
    if (!objects.glowplug_container) return;

    if (!glow_plug_on || warning_visible || showing_static_brake) {
        lv_obj_add_flag(objects.glowplug_container, LV_OBJ_FLAG_HIDDEN);
        if (objects.glowplug_icon && lv_obj_is_valid(objects.glowplug_icon))
            set_indicator_icon(objects.glowplug_icon, false, C_GP);
        if (!glow_plug_on && !warning_visible && !showing_static_brake) {
            if (objects.speed && lv_obj_has_flag(objects.speed, LV_OBJ_FLAG_HIDDEN))
                lv_obj_clear_flag(objects.speed, LV_OBJ_FLAG_HIDDEN);
            if (objects.kmh && lv_obj_has_flag(objects.kmh, LV_OBJ_FLAG_HIDDEN))
                lv_obj_clear_flag(objects.kmh, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    lv_obj_clear_flag(objects.glowplug_container, LV_OBJ_FLAG_HIDDEN);

    if (current_speed == 0) {
        if (objects.glowplug_notification)
            lv_obj_clear_flag(objects.glowplug_notification, LV_OBJ_FLAG_HIDDEN);
        if (objects.glowplug_text)
            lv_obj_clear_flag(objects.glowplug_text, LV_OBJ_FLAG_HIDDEN);
        if (objects.speed) lv_obj_add_flag(objects.speed, LV_OBJ_FLAG_HIDDEN);
        if (objects.kmh)   lv_obj_add_flag(objects.kmh, LV_OBJ_FLAG_HIDDEN);
        if (objects.glowplug_icon && lv_obj_is_valid(objects.glowplug_icon))
            set_indicator_icon(objects.glowplug_icon, false, C_GP);
    } else {
        if (objects.glowplug_notification)
            lv_obj_add_flag(objects.glowplug_notification, LV_OBJ_FLAG_HIDDEN);
        if (objects.glowplug_text)
            lv_obj_clear_flag(objects.glowplug_text, LV_OBJ_FLAG_HIDDEN);
        if (objects.speed) lv_obj_clear_flag(objects.speed, LV_OBJ_FLAG_HIDDEN);
        if (objects.kmh)   lv_obj_add_flag(objects.kmh, LV_OBJ_FLAG_HIDDEN);
        if (objects.glowplug_icon && lv_obj_is_valid(objects.glowplug_icon))
            set_indicator_icon(objects.glowplug_icon, true, C_GP);
    }
}

void dashboard_set_glow_plug(bool on) {
    if (dis_glow) return;
    glow_plug_on = on;
    update_glow_plug_display();
}

void dashboard_set_battery_warning(bool on) {
    if (!objects.battery_icon || !lv_obj_is_valid(objects.battery_icon)) return;
    lv_obj_set_style_image_recolor(objects.battery_icon, lv_color_hex(on ? C_RED : C_NORM), 0);
    lv_obj_set_style_image_opa(objects.battery_icon, 255, 0);
}

void dashboard_set_wifi_state(uint8_t state) {
    if (!objects.wifi_icon || !lv_obj_is_valid(objects.wifi_icon)) return;
    uint32_t color;
    switch (state) {
    case 1:  color = g_config.color_wifi_active;    break;
    case 2:  color = g_config.color_wifi_connected; break;
    default: color = g_config.color_wifi_off;       break;
    }
    lv_obj_set_style_image_recolor(objects.wifi_icon, lv_color_hex(color), 0);
}

// Check if brake should be static (not blinking) - when speed <= 5 and no other warnings
static bool is_brake_static(void) {
    if (!warnings_active[WARNING_BRAKE]) return false;
    if (current_speed > 5) return false;
    // Count other warnings (excluding brake)
    uint8_t other_count = 0;
    for (int i = 0; i < WARNING_COUNT; i++) {
        if (i != WARNING_BRAKE && warnings_active[i]) other_count++;
    }
    return (other_count == 0);
}

// Count warnings that should blink (excludes brake when static)
static uint8_t count_blinking_warnings(void) {
    uint8_t count = 0;
    bool brake_static = is_brake_static();
    for (int i = 0; i < WARNING_COUNT; i++) {
        if (warnings_active[i]) {
            if (i == WARNING_BRAKE && brake_static) continue;  // Skip static brake
            count++;
        }
    }
    return count;
}

// Get next active warning index
static int8_t get_next_warning(int8_t from) {
    for (int i = 1; i <= WARNING_COUNT; i++) {
        int8_t idx = (from + i) % WARNING_COUNT;
        if (warnings_active[idx]) return idx;
    }
    return -1;
}

// Add/remove warning
void warning_set(warning_type_t type, bool active) {
    if (type >= WARNING_COUNT) return;
    warnings_active[type] = active;
}

void dashboard_set_sim_flags(uint16_t flags) {
    sim_flags_dash = flags;
}

void dashboard_show_warning(const char* text) {
    if (!objects.warning_text || !objects.warning) return;
    lv_label_set_text(objects.warning_text, text);
    lv_obj_clear_flag(objects.warning, LV_OBJ_FLAG_HIDDEN);
    if (objects.speed) lv_obj_add_flag(objects.speed, LV_OBJ_FLAG_HIDDEN);
    if (objects.kmh) lv_obj_add_flag(objects.kmh, LV_OBJ_FLAG_HIDDEN);
    if (objects.notification) lv_obj_add_flag(objects.notification, LV_OBJ_FLAG_HIDDEN);
    if (objects.glowplug_container) lv_obj_add_flag(objects.glowplug_container, LV_OBJ_FLAG_HIDDEN);
}

void dashboard_hide_warning(void) {
    if (!objects.warning) return;
    lv_obj_add_flag(objects.warning, LV_OBJ_FLAG_HIDDEN);
    if (glow_plug_on) {
        update_glow_plug_display();
    } else {
        if (objects.speed) lv_obj_clear_flag(objects.speed, LV_OBJ_FLAG_HIDDEN);
        if (objects.kmh) lv_obj_clear_flag(objects.kmh, LV_OBJ_FLAG_HIDDEN);
    }
}

// ============================================================================
// FUEL SMOOTHING SYSTEM IMPLEMENTATION
// ============================================================================
static void fuel_system_process(uint8_t raw_fuel, uint16_t speed) {
    uint32_t now = lv_tick_get();
    
    // Determine target state based on speed
    fuel_state_t target_state;
    if (!fuel_system.has_displayed_value) {
        target_state = FUEL_STATE_FIRST_RUN;
    } else if (speed > 0) {
        target_state = FUEL_STATE_MOVING;
    } else {
        // Speed is 0, check if we're in delay or fully stopped
        if (fuel_system.state == FUEL_STATE_MOVING) {
            target_state = FUEL_STATE_STOPPING_DELAY;
        } else if (fuel_system.state == FUEL_STATE_STOPPING_DELAY) {
            if (now - fuel_system.state_change_time >= g_config.fuel_stop_delay_ms) {
                target_state = FUEL_STATE_STOPPED;
            } else {
                target_state = FUEL_STATE_STOPPING_DELAY;
            }
        } else {
            target_state = FUEL_STATE_STOPPED;
        }
    }
    
    // Handle state transitions
    if (target_state != fuel_system.state) {
        // If starting to move, switch immediately
        if (target_state == FUEL_STATE_MOVING) {
            fuel_system.state = FUEL_STATE_MOVING;
            fuel_system.state_change_time = now;
        }
        // If stopping, enter delay
        else if (target_state == FUEL_STATE_STOPPING_DELAY) {
            fuel_system.state = FUEL_STATE_STOPPING_DELAY;
            fuel_system.state_change_time = now;
        }
        // If delay completed, switch to stopped
        else if (target_state == FUEL_STATE_STOPPED && fuel_system.state == FUEL_STATE_STOPPING_DELAY) {
            fuel_system.state = FUEL_STATE_STOPPED;
            fuel_system.state_change_time = now;
        }
    }
    
    uint32_t sample_interval;
    switch (fuel_system.state) {
        case FUEL_STATE_FIRST_RUN:
            sample_interval = FUEL_SAMPLE_FIRST_RUN_MS;
            break;
        case FUEL_STATE_MOVING:
        case FUEL_STATE_STOPPING_DELAY:
            sample_interval = g_config.fuel_sample_moving_ms;
            break;
        case FUEL_STATE_STOPPED:
            sample_interval = g_config.fuel_sample_stopped_ms;
            break;
        default:
            sample_interval = g_config.fuel_sample_moving_ms;
            break;
    }
    
    // Check if it's time to take a sample
    // For FIRST_RUN, take sample every call (ignore interval)
    bool should_sample = (fuel_system.state == FUEL_STATE_FIRST_RUN) || 
                         (now - fuel_system.last_sample_time >= sample_interval);
    
    if (should_sample) {
        fuel_system.last_sample_time = now;
        
        // Add sample to buffer
        uint8_t buf_sz = g_config.fuel_buf_size;
        if (buf_sz < 2) buf_sz = 2;
        if (buf_sz > FUEL_BUFFER_MAX) buf_sz = FUEL_BUFFER_MAX;

        if (fuel_system.buffer_count < buf_sz) {
            fuel_system.buffer[fuel_system.buffer_count++] = raw_fuel;
        }
        
        if (fuel_system.buffer_count >= buf_sz) {
            // Calculate average
            uint32_t sum = 0;
            for (int i = 0; i < fuel_system.buffer_count; i++) {
                sum += fuel_system.buffer[i];
            }
            uint8_t avg = (uint8_t)(sum / fuel_system.buffer_count);
            
            // Clear buffer
            fuel_system.buffer_count = 0;
            
            // First run - no hysteresis, just show value
            if (!fuel_system.has_displayed_value) {
                fuel_system.displayed_value = avg;
                fuel_system.has_displayed_value = true;
                dashboard_set_fuel(avg);
                
                // Transition to appropriate state
                if (speed > 0) {
                    fuel_system.state = FUEL_STATE_MOVING;
                } else {
                    fuel_system.state = FUEL_STATE_STOPPED;
                }
                fuel_system.state_change_time = now;
            } else if (fuel_system.state == FUEL_STATE_MOVING ||
                       fuel_system.state == FUEL_STATE_STOPPING_DELAY) {
                // While driving: update display directly from buffer average
                fuel_system.displayed_value = avg;
                dashboard_set_fuel(avg);
                fuel_system.hysteresis_direction = 0;
                fuel_system.hysteresis_count = 0;
            } else {
                // While stopped: require 3 consecutive cycles in the same
                // direction before updating (filters out slope effects)
                int diff = (int)avg - (int)fuel_system.displayed_value;
                int8_t new_direction;
                
                if (diff >= g_config.fuel_hyst_threshold) {
                    new_direction = 1;  // Rising
                } else if (diff <= -(int)g_config.fuel_hyst_threshold) {
                    new_direction = -1; // Falling
                } else {
                    new_direction = 0;  // Stable (within threshold)
                }
                
                if (new_direction == 0) {
                    fuel_system.hysteresis_direction = 0;
                    fuel_system.hysteresis_count = 0;
                } else if (new_direction == fuel_system.hysteresis_direction) {
                    fuel_system.hysteresis_count++;
                    fuel_system.hysteresis_value = avg;
                    
                    if (fuel_system.hysteresis_count >= g_config.fuel_hyst_cycles) {
                        fuel_system.displayed_value = avg;
                        dashboard_set_fuel(avg);
                        fuel_system.hysteresis_direction = 0;
                        fuel_system.hysteresis_count = 0;
                    }
                } else {
                    fuel_system.hysteresis_direction = new_direction;
                    fuel_system.hysteresis_count = 1;
                    fuel_system.hysteresis_value = avg;
                }
            }
        }
    }
    
    if (fuel_system.has_displayed_value) {
        int diff = (int)raw_fuel - (int)fuel_system.displayed_value;
        if (diff < 0) diff = -diff;
        
        if (diff > g_config.fuel_emergency_pct) {
            if (!fuel_system.emergency_active) {
                fuel_system.emergency_active = true;
                fuel_system.emergency_start_time = now;
            } else if (now - fuel_system.emergency_start_time >= g_config.fuel_emergency_time_ms) {
                // Emergency confirmed - show real value immediately
                fuel_system.displayed_value = raw_fuel;
                dashboard_set_fuel(raw_fuel);
                fuel_system.buffer_count = 0;  // Clear buffer
                fuel_system.emergency_active = false;
            }
        } else {
            // Difference is within threshold, reset emergency
            fuel_system.emergency_active = false;
        }
    }
}

void dashboard_feed_fuel(uint8_t raw_pct, uint16_t speed)
{
    fuel_system_process(raw_pct, speed);
}

void dashboard_feed_temp(uint8_t raw_pct)
{
    temp_system_process(raw_pct);
}

/* PC simulator used test_input.json; on ESP data comes from main loop / HAL */
static void dashboard_timer_cb(lv_timer_t *timer) {
    (void)timer;
    
    // Process debounced inputs (3 samples @ 25ms = 75ms confirmation)
    debounce_process();

    // Periodic device failure check and reprobe (every 2 seconds)
    {
        static uint32_t last_check = 0;
        uint32_t now_chk = lv_tick_get();
        if (now_chk - last_check >= 30000) {
            last_check = now_chk;

            bool mcp  = hal_i2c_mcp_ok();
            bool bh   = hal_i2c_bh1750_ok();
            bool ads  = hal_adc_ok();

            if (!mcp) hal_i2c_mcp_reprobe();
            if (!bh)  hal_i2c_bh1750_reprobe();
            if (!ads) hal_adc_reprobe();

            mcp = hal_i2c_mcp_ok();
            bh  = hal_i2c_bh1750_ok();
            ads = hal_adc_ok();

            if (mcp && bh && ads) {
                if (objects.dashboard_failures &&
                    !lv_obj_has_flag(objects.dashboard_failures, LV_OBJ_FLAG_HIDDEN)) {
                    lv_label_set_text(objects.dashboard_failures, "");
                    lv_obj_add_flag(objects.dashboard_failures, LV_OBJ_FLAG_HIDDEN);
                }
            } else {
                char buf[128];
                int pos = 0;
                if (!mcp) pos += snprintf(buf + pos, sizeof(buf) - pos, "MCP23017 FAILURE!\n");
                if (!ads) pos += snprintf(buf + pos, sizeof(buf) - pos, "ADS1115 FAILURE!\n");
                if (!bh)  pos += snprintf(buf + pos, sizeof(buf) - pos, "BH1750 FAILURE!\n");
                if (pos > 0 && buf[pos - 1] == '\n') buf[pos - 1] = '\0';

                if (objects.dashboard_failures) {
                    lv_label_set_text(objects.dashboard_failures, buf);
                    lv_obj_clear_flag(objects.dashboard_failures, LV_OBJ_FLAG_HIDDEN);
                    lv_obj_move_foreground(objects.dashboard_failures);
                }
            }
        }
    }

    // Force-activate/deactivate warnings from simulation flags
    static uint16_t prev_sim = 0;
    uint16_t cleared = prev_sim & ~sim_flags_dash;
    for (int i = 0; i < WARNING_COUNT; i++) {
        if (sim_flags_dash & (1 << i))
            warnings_active[i] = true;
        else if (cleared & (1 << i))
            warnings_active[i] = false;
    }
    prev_sim = sim_flags_dash;

    // Shared blink timer - wall-clock based (1000ms on, 500ms off)
    static uint32_t blink_last_change = 0;
    uint32_t blink_now = lv_tick_get();
    uint32_t phase_dur = blink_on ? g_config.blink_on_ms : g_config.blink_off_ms;
    bool phase_changed = false;
    if (blink_now - blink_last_change >= phase_dur) {
        blink_last_change = blink_now;
        blink_on = !blink_on;
        phase_changed = true;
    }
    bool transition_to_off = phase_changed && !blink_on;
    bool transition_to_on = phase_changed && blink_on;
    
    // RPM redline fast blink (50ms on, 50ms off = 100ms cycle)
    {
        static uint8_t rpm_blink_counter = 0;
        rpm_blink_counter++;
        if (rpm_blink_counter >= 4) rpm_blink_counter = 0;  // 4 ticks * 25ms = 100ms cycle
        
        if (objects.led6 && lv_obj_is_valid(objects.led6)) {
            if (current_tach_level == 6) {
                lv_obj_set_style_image_recolor(objects.led6, (rpm_blink_counter < 2) ? lv_color_hex(C_RED) : lv_color_hex(C_DIM), 0);
            }
        }
    }
    
    // HOT blink when temp exceeds threshold
    {
        static bool overheating_active = false;
        static uint32_t overheating_start = 0;
        if (current_temp > g_config.temp_overheat_pct) {
            if (phase_changed && objects.temp_status && lv_obj_is_valid(objects.temp_status)) {
                lv_obj_set_style_text_opa(objects.temp_status, blink_on ? 255 : 0, 0);
            }
            if (!overheating_active) {
                if (overheating_start == 0) {
                    overheating_start = lv_tick_get();
                } else if (lv_tick_get() - overheating_start >= g_config.temp_overheat_delay_ms) {
                    warning_set(WARNING_OVERHEATING, true);
                    overheating_active = true;
                }
            }
        } else {
            if (overheating_active) {
                warning_set(WARNING_OVERHEATING, false);
                overheating_active = false;
                if (objects.temp_status && lv_obj_is_valid(objects.temp_status)) {
                    lv_obj_set_style_text_opa(objects.temp_status, 255, 0);
                }
            }
            overheating_start = 0;
        }
    }
    
    // Voltage text blink when battery warnings active (update only on phase change)
    if (phase_changed) {
        bool battery_warning_active = warnings_active[WARNING_LOW_BATTERY] || warnings_active[WARNING_OVERVOLTAGE];
        if (battery_warning_active) {
            if (objects.voltage && lv_obj_is_valid(objects.voltage)) {
                lv_obj_set_style_text_opa(objects.voltage, blink_on ? 255 : 0, 0);
            }
        } else {
            if (objects.voltage && lv_obj_is_valid(objects.voltage)) {
                lv_obj_set_style_text_opa(objects.voltage, 255, 0);
            }
        }
    }
    
    // Warning display logic
    uint8_t blinking_count = count_blinking_warnings();
    bool brake_static = is_brake_static();
    
    // Handle static brake (speed <= 5, only brake active)
    if (brake_static && !showing_static_brake) {
        if (objects.warning_text && lv_obj_is_valid(objects.warning_text)) {
            lv_label_set_text(objects.warning_text, warning_texts[WARNING_BRAKE]);
            lv_obj_set_style_text_opa(objects.warning_text, 255, 0);
        }
        if (objects.warning) lv_obj_clear_flag(objects.warning, LV_OBJ_FLAG_HIDDEN);
        if (objects.speed) lv_obj_add_flag(objects.speed, LV_OBJ_FLAG_HIDDEN);
        if (objects.kmh) lv_obj_add_flag(objects.kmh, LV_OBJ_FLAG_HIDDEN);
        if (objects.notification) lv_obj_add_flag(objects.notification, LV_OBJ_FLAG_HIDDEN);
        if (objects.glowplug_container) lv_obj_add_flag(objects.glowplug_container, LV_OBJ_FLAG_HIDDEN);
        showing_static_brake = true;
        warning_visible = true;
    } else if (!brake_static && showing_static_brake) {
        showing_static_brake = false;
    }
    
    // Handle static brake deactivation
    if (showing_static_brake && !warnings_active[WARNING_BRAKE]) {
        if (objects.warning) lv_obj_add_flag(objects.warning, LV_OBJ_FLAG_HIDDEN);
        showing_static_brake = false;
        warning_visible = false;
        if (glow_plug_on) {
            update_glow_plug_display();
        } else {
            if (objects.speed) lv_obj_clear_flag(objects.speed, LV_OBJ_FLAG_HIDDEN);
            if (objects.kmh) lv_obj_clear_flag(objects.kmh, LV_OBJ_FLAG_HIDDEN);
        }
        if (objects.notification && (fuel_warning_on || (sim_flags_dash & SIM_LOW_FUEL))) lv_obj_clear_flag(objects.notification, LV_OBJ_FLAG_HIDDEN);
    }
    
    // Blink logic for warnings (skip if static brake)
    if (!showing_static_brake) {
        // Immediate cleanup when all warnings gone (don't wait for blink phase)
        if (warning_visible && blinking_count == 0) {
            if (objects.warning && !lv_obj_has_flag(objects.warning, LV_OBJ_FLAG_HIDDEN))
                lv_obj_add_flag(objects.warning, LV_OBJ_FLAG_HIDDEN);
            if (objects.warning_text) lv_obj_set_style_text_opa(objects.warning_text, 255, 0);
            warning_visible = false;
            if (glow_plug_on) {
                update_glow_plug_display();
            } else {
                if (objects.speed && lv_obj_has_flag(objects.speed, LV_OBJ_FLAG_HIDDEN))
                    lv_obj_clear_flag(objects.speed, LV_OBJ_FLAG_HIDDEN);
                if (objects.kmh && lv_obj_has_flag(objects.kmh, LV_OBJ_FLAG_HIDDEN))
                    lv_obj_clear_flag(objects.kmh, LV_OBJ_FLAG_HIDDEN);
            }
        }

        // Transition to OFF phase - cycle warning text
        if (transition_to_off && blinking_count > 0) {
            if (blinking_count > 1) {
                int8_t next = get_next_warning(current_warning_index);
                while (next >= 0 && next == WARNING_BRAKE && brake_static) {
                    next = get_next_warning(next);
                }
                if (next >= 0) {
                    current_warning_index = next;
                    if (objects.warning_text && lv_obj_is_valid(objects.warning_text))
                        lv_label_set_text(objects.warning_text, warning_texts[current_warning_index]);
                }
            } else if (!warnings_active[current_warning_index]) {
                int8_t next = get_next_warning(current_warning_index);
                if (next >= 0) {
                    current_warning_index = next;
                    if (objects.warning_text && lv_obj_is_valid(objects.warning_text))
                        lv_label_set_text(objects.warning_text, warning_texts[current_warning_index]);
                }
            }
        }
        
        // Transition to ON phase - show warning, hide speed
        if (transition_to_on && blinking_count > 0 && !warning_visible) {
            if (!warnings_active[current_warning_index] || (current_warning_index == WARNING_BRAKE && brake_static)) {
                int8_t next = get_next_warning(-1);
                while (next >= 0 && next == WARNING_BRAKE && brake_static) {
                    next = get_next_warning(next);
                }
                if (next >= 0) current_warning_index = next;
            }
            if (objects.warning_text && lv_obj_is_valid(objects.warning_text))
                lv_label_set_text(objects.warning_text, warning_texts[current_warning_index]);
            if (objects.warning) lv_obj_clear_flag(objects.warning, LV_OBJ_FLAG_HIDDEN);
            if (objects.speed) lv_obj_add_flag(objects.speed, LV_OBJ_FLAG_HIDDEN);
            if (objects.kmh) lv_obj_add_flag(objects.kmh, LV_OBJ_FLAG_HIDDEN);
            if (objects.notification) lv_obj_add_flag(objects.notification, LV_OBJ_FLAG_HIDDEN);
            if (objects.glowplug_container) lv_obj_add_flag(objects.glowplug_container, LV_OBJ_FLAG_HIDDEN);
            warning_visible = true;
        }
        
        // Blink warning text opacity (icon stays visible, mask covers background)
        if (phase_changed && warning_visible && objects.warning_text && lv_obj_is_valid(objects.warning_text)) {
            lv_obj_set_style_text_opa(objects.warning_text, blink_on ? 255 : 0, 0);
        }
    }
    
    // Sync fuel_warning with speedometer (only update on change to avoid redundant invalidations)
    if (objects.notification) {
        bool fuel_should_show = (fuel_warning_on || (sim_flags_dash & SIM_LOW_FUEL)) && !warning_visible;
        bool is_hidden = lv_obj_has_flag(objects.notification, LV_OBJ_FLAG_HIDDEN);
        if (fuel_should_show && is_hidden) {
            lv_obj_clear_flag(objects.notification, LV_OBJ_FLAG_HIDDEN);
        } else if (!fuel_should_show && !is_hidden) {
            lv_obj_add_flag(objects.notification, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void dashboard_process_hal_samples(uint16_t speed_kmh, uint8_t fuel_pct,
                                   uint8_t temp_smooth_val, bool voltage_valid, float voltage_raw)
{
    dashboard_set_speed(speed_kmh);
    fuel_system_process(fuel_pct, speed_kmh);
    temp_system_process(temp_smooth_val);
    dashboard_feed_voltage(voltage_valid, voltage_raw);
}

void dashboard_start(void) {
    if (dashboard_timer == NULL) {
        dashboard_timer = lv_timer_create(dashboard_timer_cb, 25, NULL);
    }
}

void dashboard_stop(void) {
    if (dashboard_timer != NULL) {
        lv_timer_delete(dashboard_timer);
        dashboard_timer = NULL;
    }
}