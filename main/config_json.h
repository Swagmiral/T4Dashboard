#ifndef CONFIG_JSON_H
#define CONFIG_JSON_H

#include <stdbool.h>
#include "config.h"

/**
 * Serialize g_config (+ optional odometer for HTTP backup).
 * Caller must free() the returned string.
 */
char *dashboard_config_to_json(bool include_odometer);

/**
 * Merge recognized JSON keys into g_config. Unknown keys are ignored.
 * @param apply_odometer  If true and "odometer_km" is present, updates g_odometer_km + NVS slots.
 * After merge, applies config_apply_forced_defaults().
 */
bool dashboard_config_from_json(const char *json, bool apply_odometer);

/**
 * Keys listed in config_json.c (`forced_default_keys` + loop body) are reset to
 * firmware defaults after any JSON merge (boot, POST, restore).
 */
void config_apply_forced_defaults(dashboard_config_t *cfg);

#endif
