/*
 * relays.h - output control with compressor protection.
 *
 * The raw pins are trivial; the value here is the interlocking. A fridge
 * compressor restarted against head pressure can stall and burn out its
 * windings, so short-cycling is not a cosmetic concern. The stock firmware
 * exposes `compressorDelay` and `modeSwitchDelay` as device-twin properties
 * for exactly this reason, and this module enforces the same guarantees:
 *
 *   - heater and cooler can never be energised simultaneously
 *   - the compressor observes a minimum off time before restarting
 *   - the compressor observes a minimum run time before stopping
 *   - a dwell time is enforced when switching between heating and cooling
 *
 * Requests that violate a guard are refused and reported, not silently
 * queued - the caller decides what to do about it.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    RELAY_HEATER = 0,
    RELAY_COOLER,
    RELAY_FAN,
    RELAY_LIGHT,
    RELAY_BUZZER,
    RELAY_COUNT
} relay_t;

typedef struct {
    uint32_t compressor_min_off_s;  /* refuse restart until this has elapsed */
    uint32_t compressor_min_on_s;   /* refuse stop until this has elapsed    */
    uint32_t mode_switch_delay_s;   /* dwell between heating and cooling     */
} relays_config_t;

/* Conservative defaults. Typical domestic compressors want 3-5 minutes off. */
#define RELAYS_DEFAULT_CONFIG() ((relays_config_t){ \
    .compressor_min_off_s = 300,                    \
    .compressor_min_on_s  = 120,                    \
    .mode_switch_delay_s  = 60,                     \
})

esp_err_t relays_init(const relays_config_t *cfg);

/* Reconfigure at runtime - e.g. when new twin properties arrive. */
void relays_configure(const relays_config_t *cfg);

/*
 * Unguarded control. Appropriate for fan, light and buzzer.
 * Returns ESP_ERR_INVALID_ARG for heater/cooler - use the guarded calls.
 */
esp_err_t relays_set(relay_t r, bool on);
bool relays_get(relay_t r);

/*
 * Guarded control for the temperature-affecting outputs.
 *
 * Returns ESP_OK if the state was applied, or ESP_ERR_INVALID_STATE if a
 * protection guard refused it. Refusal is normal and expected - call again
 * on your next control cycle.
 */
esp_err_t relays_request_cool(bool on);
esp_err_t relays_request_heat(bool on);

/* Seconds until a refused request would be permitted, 0 if permitted now. */
uint32_t relays_cool_blocked_for(void);
uint32_t relays_heat_blocked_for(void);

/*
 * Permit heater and cooler to be energised together - REHEAT DEHUMIDIFICATION.
 *
 * Off by default, and it should stay off unless something is actively
 * managing both. It exists because running the compressor to drag the
 * evaporator below dew point, while the heater cancels the resulting sensible
 * cooling, is the standard way to remove moisture without moving temperature.
 * It is how commercial curing chambers work.
 *
 * It is only sane on a chamber whose evaporator DRAINS. If the coil merely
 * frosts, the water returns at the next defrost and all this achieves is a
 * frosted coil and a large electricity bill.
 *
 * The compressor's minimum off time and minimum run time still apply. What
 * this relaxes is only the mutual exclusion and the heat/cool dwell.
 *
 * The caller is responsible for bounding it - a temperature envelope, a
 * duration cap, and a valid sensor. Two opposing actuators running against
 * each other turn a stuck sensor from an annoyance into a hazard.
 */
void relays_allow_simultaneous(bool allow);
bool relays_simultaneous_allowed(void);

/*
 * Immediate de-energise of heater and cooler, bypassing minimum run time.
 * For fault conditions: probe failure, over-temperature, shutdown.
 */
void relays_emergency_stop(void);

/* Counters matching the stock telemetry fields. */
uint32_t relays_runtime_seconds(relay_t r);
uint32_t relays_start_count(relay_t r);
uint32_t relays_total_runtime_seconds(void);
