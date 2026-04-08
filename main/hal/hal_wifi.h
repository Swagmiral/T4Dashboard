#ifndef HAL_WIFI_H
#define HAL_WIFI_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    WIFI_STATE_OFF = 0,
    WIFI_STATE_ACTIVE,
    WIFI_STATE_CONNECTED,
} wifi_state_t;

#define SIM_LOW_BATTERY   (1 << 0)
#define SIM_OVERVOLTAGE   (1 << 1)
#define SIM_OVERHEATING   (1 << 2)
#define SIM_BRAKE         (1 << 3)
#define SIM_OIL_PRESSURE  (1 << 4)
#define SIM_LOW_FUEL      (1 << 5)
#define SIM_GLOW_PLUG     (1 << 6)
#define SIM_HIGH_BEAM     (1 << 7)
#define SIM_BLINKER_L     (1 << 8)
#define SIM_BLINKER_R     (1 << 9)

typedef struct {
    uint16_t speed_kmh;
    uint16_t rpm;
    float voltage;
    float fuel_v;
    uint8_t fuel_pct;
    float temp_c;
    uint8_t temp_pct;
    float lux;
    uint8_t brightness;
    uint16_t mcp_inputs;
    bool mcp_ok;
    bool ignition;
} live_sensors_t;

void hal_wifi_init(void);
wifi_state_t hal_wifi_get_state(void);
uint16_t hal_wifi_get_sim_flags(void);
void hal_wifi_update_sensors(const live_sensors_t *s);

#endif
