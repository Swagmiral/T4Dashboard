# T4 Dashboard Project Context

## Project Overview
Custom digital instrument cluster for a 1998 VW Transporter T4 1.9TD, replacing the original failing analog cluster. Built on ESP32-S3 with a TFT display using LVGL graphics library. UI designed in EEZ Studio.

## Hardware Setup
- **MCU**: ESP32-S3 (with PSRAM)
- **Display**: TFT LCD driven via ESP LCD + LVGL
- **I2C Bus**: MCP23017 (GPIO expander), BH1750 (light sensor), ADS1115 (16-bit ADC)
- **MCP23017**: Reads digital inputs — ignition, blinkers (L pin 6, R pin 5), glow plug, high beam, etc. Active-low inputs via optocouplers.
- **ADS1115**: Reads analog signals — battery voltage (ch0), fuel level (ch1), coolant temperature (ch2)
- **Speed Sensor**: VW 357919149 (3-pin Hall effect, open-collector output) on gearbox, signal on cluster pin 27 (NOT pin 28), through 74HC14 Schmitt trigger to GPIO43
- **Tachometer**: Alternator W terminal signal on GPIO44
- **MOSFET Self-Latch**: GPIO6 controls power latch for delayed shutdown after ignition off
- **WiFi**: SoftAP mode for configuration via HTTP page
- **Flashing**: Via UART1, then switch to UART2 (GPIO43/44 shared with speed/tach)

## Pin Assignments
- GPIO43: Speed sensor input (via 74HC14)
- GPIO44: Tachometer input
- GPIO6: MOSFET self-latch control
- I2C SDA/SCL: For MCP23017, BH1750, ADS1115

## Speed Sensor Circuit (CURRENT - as of April 2026)
The speed sensor (357919149) is a Hall effect sensor with **open-collector output**. It needs an external pull-up resistor since the original instrument cluster (which provided the internal pull-up) has been replaced.

### Circuit:
```
3.3V ─── 4.7k pull-up ──┐
                         P27 (cluster pin 27) → 4.7k series → 74HC14 pin 1 (1A)
                         74HC14 pin 1 → 22nF → GND          (RC filter)
                         74HC14 pin 1 → anode 1N4148 → 3.3V (overvoltage clamp)
                         74HC14 pin 1 → cathode 1N4148 → GND (undervoltage clamp)
                         74HC14 pin 2 (1Y) → 10k → GPIO43
                         GPIO43 → 10k + 5.1k → GND
                         74HC14 VCC → 3.3V
                         74HC14 GND → GND
                         All unused 74HC14 inputs tied to GND
```

### Key Discovery (April 2026):
The original circuit had NO pull-up resistor. The 357919149 Hall sensor has an open-collector output that can only pull LOW — it needs an external pull-up to create the HIGH state. The original VW instrument cluster provided this internally. Without it, the signal line floated and the 74HC14 saw no edges.

**Previous symptoms**: Speedometer worked on bench (tapping 12V directly), but not in car. Pin 27 showed ~0V against ground (floating) but -6V to -8V against 12V (confirming signal was present but no pull-up).

**Fix applied**: 4.7k pull-up from P27 wire to 3.3V, placed BEFORE the 4.7k series resistor (important — if placed after, the two 4.7k resistors form a voltage divider that sits at ~1.65V when sensor is active, which is marginal for the 74HC14 threshold).

**STATUS**: Pull-up moved to correct position (before series resistor). Not yet tested in car.

## 74HC14 History
- Originally had floating inputs causing overheating
- Added 10k pull-down to input (later identified as wrong — needs pull-up)
- Added pull-down resistors on unused inputs to GND — this was correct
- The 74HC14 input previously showed 1.2-2V floating voltage, causing overheating. Adding the pull-down and tying unused inputs to GND fixed the overheating.

## Firmware Architecture
- **main.c**: Main loop — reads sensors, updates UI, manages power state
- **hal_speed.c**: Speed sensor ISR + median filter + rate-of-change limiter
- **hal_tach.c**: Tachometer input
- **hal_i2c.c**: I2C bus init + MCP23017/BH1750/ADS1115 communication
- **hal_adc.c**: ADS1115 ADC driver
- **hal_nvs.c**: NVS config storage with validation/sanitization
- **hal_wifi.c**: WiFi SoftAP + HTTP config server + live sensor endpoint
- **hal_display.c**: Display driver + LVGL tick
- **dashboard.c**: LVGL UI — speedometer arc+label, fuel, temp, indicators
- **config.h**: Configuration struct (CONFIG_VERSION = 14)

## Key Firmware Features & Fixes Applied

### Speed Processing (hal_speed.c)
- Circular buffer (PBUF_MAX=9) for median filtering of ISR periods
- Runtime-configurable filter depth via `g_config.speed_filter_size` (default 5, max 9)
- Rate-of-change limiter: rejects speed jumps exceeding `speed_max_accel` km/h/s unless confirmed by `speed_confirm_count` consecutive samples in the same direction
- Division-by-zero guard on `pulses_per_km`
- `gpio_reset_pin()` calls REMOVED (were causing issues)
- ISR uses ANYEDGE — reads both rising and falling edges
- Static buffer in `hal_speed_get_kmh()` to reduce stack usage

### Display (dashboard.c)
- Speed label: hysteresis via `speed_text_hyst`, but always updates immediately to 0
- Speed arc: EMA smoothing, snaps to 0 when `arc_ema < 0.5f`
- Temperature: configurable display hysteresis (`temp_display_hyst`, default 2)
- Glow plug indicator: only toggles visibility on speed state change (0↔non-zero)
- No early return on `kmh == prev_speed` — each display element manages its own update

### ADC Processing (main.c)
- `adc_settle[3]` counter: skips first 2 readings per channel after boot to avoid stale data
- Voltage: validated 5.0-24.0V range
- Fuel: learning only active when `speed_kmh > 0` (prevents false range during standstill)
- Temperature: NTC Steinhart-Hart calculation with guards for division-by-zero and NaN
- All ADC values pushed to `live_sensors_t` for WiFi display

### WiFi (hal_wifi.c)
- SoftAP with HTTP config page
- `/sensors` JSON endpoint for live sensor data (polled every 2s by JS)
- Live values displayed on first line of each config group
- DNS task properly terminated on WiFi shutdown (`dns_sock` closed, task handle tracked)
- WiFi idle timeout triggers `wifi_shutdown()` which cleanly stops DNS task
- Config page includes speed_filter_size, speed_max_accel, speed_confirm_count, temp_display_hyst

### NVS Config (hal_nvs.c)
- CONFIG_VERSION = 14 (bump required when adding fields)
- Post-load sanitization prevents crashes from bad values:
  - pulses_per_km: min 539
  - temp_r_nominal: min 2500.0
  - temp_beta: min 3435.0
  - speed_filter_size: 1-9
  - speed_confirm_count: min 1
  - tach_pulses_per_rev: min 1
  - voltage_multiplier: min 0.01

### Power Management
- MOSFET self-latch on GPIO6
- Delayed shutdown (5s) after ignition loss
- Brownout threshold lowered to level 3 (~2.6V) in sdkconfig.defaults

### Stack & Memory
- Main task stack: 8192 bytes (increased from 3584 to fix overflow)
- Static buffers in speed calculation to reduce stack pressure

## sdkconfig.defaults Key Settings
- `CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192`
- `CONFIG_ESP_BROWNOUT_DET_LVL_SEL_3=y` (lowered from level 7)
- Log level: WARN default, INFO max
- Console: default (UART0) — DO NOT change to USB-JTAG or NONE
- **IMPORTANT**: Always `del sdkconfig` before building after changing sdkconfig.defaults

## Resolved Hardware Issues
1. **Optocoupler ground desoldered** → Fixed blinkers and glow plug (user resoldered)
2. **74HC14 floating inputs** → Tied unused inputs to GND, added pull-down (now pull-up)
3. **74HC14 overheating** → Caused by floating input oscillating in undefined region
4. **I2C bus stuck (SDA=0 SCL=0)** → Removed manual `i2c_bus_recovery()` from init
5. **Boot loops from USB power** → Brownout from insufficient current; use lab PSU or car 12V
6. **Boot loops from bad NVS config** → Added sanitization guards; fix requires `idf.py erase-flash`
7. **Stack overflow** → Increased main task stack to 8192, made speed buffer static
8. **DNS task crash on WiFi shutdown** → Properly close dns_sock and wait for task exit
9. **Speed label stuck at non-zero** → Fixed hysteresis to always allow update to 0
10. **Fuel reads 3-5% then 0 at boot** → Added adc_settle counter (skip first 2 readings)

## Current Status (April 2026)
- **Working**: Display, blinkers, fuel gauge, temperature, high beam, voltage, glow plug, WiFi config page with live sensors
- **Speedometer**: Works on bench (tapping), pull-up resistor fix applied but NOT YET TESTED IN CAR
- **Tachometer**: Not yet verified (alternator W terminal)
- **Git repo**: https://github.com/Swagmiral/T4Dashboard (just pushed, all current code)

## Things NOT to Do
- Do NOT add `gpio_reset_pin()` calls — they were removed intentionally
- Do NOT change console to USB-JTAG or NONE in sdkconfig
- Do NOT add `i2c_bus_recovery()` to hal_i2c_init — it caused stuck bus
- Do NOT skip `del sdkconfig` when changing sdkconfig.defaults
- Do NOT put the pull-up resistor AFTER the 4.7k series resistor (voltage divider issue)

## VW T4 1.9TD Specific Info
- Speed sensor: 357919149 (3-pin Hall effect, open-collector)
  - Pin 1: 12V power from fuse 15 (ignition-switched)
  - Pin 2: Signal output (open-collector, needs external pull-up)
  - Pin 3: Ground (brown wire)
- Signal goes to instrument cluster pin 27 on 28-pin connector (NOT pin 28)
- Pin numbering (plug removed, front facing you): top-left=1, top-right=14, bottom-left=15, bottom-right=28
- Tachometer uses alternator W terminal signal
- 1.9TD is mechanical injection diesel (not electronic TDI)
