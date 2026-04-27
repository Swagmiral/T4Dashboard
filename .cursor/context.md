# T4 Dashboard Project Context

## Project Overview
Custom digital instrument cluster for a 1998 VW Transporter T4 1.9TD, replacing the original failing analog cluster. Built on ESP32-S3 with a TFT display using LVGL graphics library. UI designed in EEZ Studio.

## Hardware Setup
- **MCU**: ESP32-S3 (with PSRAM)
- **Display**: TFT LCD driven via ESP LCD + LVGL
- **I2C Bus**: MCP23017 (GPIO expander), BH1750 (light sensor), ADS1115 (16-bit ADC)
- **MCP23017**: Reads digital inputs — ignition, blinkers (L pin 6, R pin 5), glow plug, high beam, etc. Active-low inputs via optocouplers.
- **ADS1115**: Reads analog signals — fuel level (AIN0), coolant temperature (AIN1), battery voltage (AIN3). **NOTE**: AIN1/AIN2 are internally shorted on this chip (counterfeit/faulty). Voltage was moved from AIN2 to AIN3 as workaround. Temp on AIN1 still has parasitic coupling from the damaged MUX. Replace with genuine TI ADS1115IDGSR.
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
- **hal_nvs.c**: NVS config storage with validation/sanitization (CONFIG_VERSION = 15)
- **hal_wifi.c**: WiFi SoftAP + HTTP config server + live sensor endpoint
- **hal_display.c**: Display driver + LVGL tick
- **dashboard.c**: LVGL UI — speedometer arc+label, fuel, temp, indicators
- **config.h**: Configuration struct (CONFIG_VERSION = 15)

## Key Firmware Features & Fixes Applied

### Speed Processing (hal_speed.c)
- **PCNT hardware pulse counter** with configurable glitch filter (`speed_glitch_ns`, default 1000ns)
- **GPIO ISR software debounce** rejects pulses closer than `speed_min_period_us` (default 1500µs)
- **Frequency-based speed**: counts accepted pulses in a sliding window (`speed_window_ms`, default 500ms), calculates speed from time span between first and last pulse
- **Low speed handling**: when 0-1 pulses in window, uses elapsed time since last pulse as upper-bound estimate (smooth decay toward zero)
- **No mode switching**: single unified formula works at all speeds without discontinuity
- **PCNT raw count** drives odometer for accurate distance measurement
- **Stopped detection**: `speed_stopped_ms` (default 2000ms) no-pulse timeout
- Division-by-zero guard on `pulses_per_km`
- `gpio_reset_pin()` calls REMOVED (were causing issues)

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
- CONFIG_VERSION = 15 (bump required when adding fields)
- Post-load sanitization prevents crashes from bad values:
  - pulses_per_km: min 539
  - temp_r_nominal: min 2500.0
  - temp_beta: min 3435.0
  - speed_window_ms: min 1
  - speed_stopped_ms: min 1
  - tach_pulses_per_rev: min 1
  - voltage_multiplier: min 0.01

### Power Management
- MOSFET self-latch on GPIO6
- Delayed shutdown (5s) after ignition loss
- Brownout threshold lowered to level 3 (~2.6V) in sdkconfig.defaults

### Stack & Memory
- Main task stack: 8192 bytes (increased from 3584 to fix overflow)
- Static buffers in speed calculation to reduce stack pressure

### LVGL Memory Allocator Fix (April 2026)
- **Problem**: LVGL used its built-in allocator (`LV_USE_BUILTIN_MALLOC`) with a **64KB fixed pool** in internal SRAM. The LVGL bin decoder (`decode_alpha_only` in `lv_bin_decoder.c`) always copies alpha-only images (A4/A8) into a new RAM buffer — even A8 where no conversion is needed. The speedmask image (300x300 A8 = 90KB) exceeded the 64KB pool, causing a silent allocation failure and invisible rendering.
- **Root cause**: `lv_draw_buf_create()` → `lv_malloc()` → LVGL builtin pool (64KB) → fails for 90KB → decoder returns `LV_RESULT_INVALID` → image widget has no content → invisible.
- **Fix**: Switched to C stdlib: `CONFIG_LV_USE_CLIB_MALLOC=y` in `sdkconfig.defaults`. With `CONFIG_SPIRAM_USE_MALLOC=y` and `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096`, allocations >4KB automatically go to PSRAM. The 90KB speedmask buffer now allocates in PSRAM successfully.
- **Side benefit**: Freed ~64KB of internal SRAM (.bss dropped from ~82KB to ~17KB) that was previously reserved for LVGL's static pool.
- **Why smaller images worked**: test5 (200x200 A4 → decoded to 200x200 A8 = 40KB) fit in the 64KB pool. Only images >64KB were affected.

### PSRAM SPI Bus Contention Fix (April 2026)
- **Problem**: `CONFIG_SPIRAM_RODATA=y` and `CONFIG_SPIRAM_FETCH_INSTRUCTIONS=y` placed read-only data and code in PSRAM, competing with LCD DMA for the PSRAM SPI bus, causing rendering glitches.
- **Fix**: Disabled both in `sdkconfig.defaults` — const image data stays in flash, code executes from IRAM/flash cache, only framebuffers and heap use PSRAM.

## sdkconfig.defaults Key Settings
- `CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192`
- `CONFIG_ESP_MAIN_TASK_PRIORITY=6`
- `CONFIG_ESP_BROWNOUT_DET_LVL_SEL_3=y` (lowered from level 7)
- `CONFIG_LV_USE_CLIB_MALLOC=y` (use C stdlib, NOT LVGL builtin 64KB pool)
- `CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM=y` (critical LVGL code in IRAM)
- `CONFIG_COMPILER_OPTIMIZATION_PERF=y`
- `# CONFIG_SPIRAM_RODATA is not set` (keep rodata in flash, not PSRAM)
- `# CONFIG_SPIRAM_FETCH_INSTRUCTIONS is not set` (keep code in flash, not PSRAM)
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
11. **Speedmask image invisible** → LVGL 64KB builtin allocator too small for 90KB decoded A8 image; switched to `CONFIG_LV_USE_CLIB_MALLOC=y` so large allocs go to PSRAM
12. **PSRAM bus contention causing rendering glitches** → Disabled `SPIRAM_RODATA` and `SPIRAM_FETCH_INSTRUCTIONS` in sdkconfig.defaults
13. **A8 image decode wastes PSRAM** → LVGL `decode_alpha_only` copies A8 C-arrays to PSRAM unnecessarily; patched `lv_bin_decoder.c` to use const data directly from flash (zero-copy) for A8 format
14. **Speed label constant invalidation** → `lv_label_set_text_fmt` always invalidates even for same text; added `if (kmh == displayed_speed) return` guard in `dashboard_set_speed`
15. **ADS1115 counterfeit — AIN1/AIN2 internal MUX short** → Diagnosed via raw ADC logging through `/sensors` endpoint. Both AIN1 and AIN2 returned identical voltages (~20mV offset). Disconnecting NTC from AIN1 caused BOTH channels to jump to 3.2V (pullup voltage). Confirmed chip's internal MUX is dead between those two adjacent pins. **Workaround**: moved battery voltage from AIN2 to AIN3 (config: `adc_voltage_ch=3`). Voltage and fuel now read correctly. Temp on AIN1 still shows ~18% parasitic coupling to battery voltage changes through the damaged MUX. Current channel layout: AIN0=fuel, AIN1=temp (degraded), AIN2=unused (dead), AIN3=voltage.

## Current Status (April 2026)
- **Working**: Display, blinkers, fuel gauge, temperature, high beam, voltage, glow plug, WiFi config page with live sensors, speedmask rendering
- **Speedometer**: Works on bench (tapping), pull-up resistor fix applied but NOT YET TESTED IN CAR
- **Tachometer**: Not yet verified (alternator W terminal)
- **Known issue**: Temp gauge still has residual voltage dependency (~18% reading shift for ~3% battery change) due to damaged AIN1 on counterfeit ADS1115. Fuel (AIN0) and voltage (AIN3) are unaffected. Full fix requires replacing the ADS1115 chip with genuine TI part.
- **Fixed**: Speed text + KM/H flashing — A8 zero-copy patch in bin decoder + early-return guard in `dashboard_set_speed` to stop constant label invalidation
- **Git repo**: https://github.com/Swagmiral/T4Dashboard (just pushed, all current code)

## Things NOT to Do
- Do NOT add `gpio_reset_pin()` calls — they were removed intentionally
- Do NOT change console to USB-JTAG or NONE in sdkconfig
- Do NOT add `i2c_bus_recovery()` to hal_i2c_init — it caused stuck bus
- Do NOT skip `del sdkconfig` when changing sdkconfig.defaults
- Do NOT put the pull-up resistor AFTER the 4.7k series resistor (voltage divider issue)

## AI Assistant
- **Model**: Claude Opus 4.6 (Anthropic) — used via Cursor IDE
- **Chat continuity**: On a new PC/chat, say "read `.cursor/context.md` and continue from where we left off"

## VW T4 1.9TD Specific Info
- Speed sensor: 357919149 (3-pin Hall effect, open-collector)
  - Pin 1: 12V power from fuse 15 (ignition-switched)
  - Pin 2: Signal output (open-collector, needs external pull-up)
  - Pin 3: Ground (brown wire)
- Signal goes to instrument cluster pin 27 on 28-pin connector (NOT pin 28)
- Pin numbering (plug removed, front facing you): top-left=1, top-right=14, bottom-left=15, bottom-right=28
- Tachometer uses alternator W terminal signal
- 1.9TD is mechanical injection diesel (not electronic TDI)
