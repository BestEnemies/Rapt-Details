/*
 * thermostat.h - hysteresis temperature control for the chamber.
 *
 * Deliberately bang-bang with a deadband, not PID. The stock firmware exposes
 * pid_p/pid_i/pid_d, but a fridge compressor is a fixed-output device: PID
 * only helps if you can modulate, and modulating a compressor with PWM is how
 * you destroy it. Hysteresis with a sensible deadband is the correct control
 * law here.
 *
 * Cut-in / cut-out, rather than a single threshold:
 *
 *   cooling starts when  temp > target + cooler_hys
 *   cooling stops  when  temp <= target
 *   heating starts when  temp < target - heater_hys
 *   heating stops  when  temp >= target
 *
 * Stopping at the target rather than at the opposite threshold means each
 * cycle actually reaches the setpoint instead of oscillating between the two
 * trip points, and it keeps the compressor off for as long as possible.
 *
 * This layer decides *intent*. Whether the compressor may actually start is
 * still the relay layer's call - minimum off time, minimum run time and the
 * heat/cool dwell all apply, and a refused request is normal, not an error.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "alarms.h"

typedef enum {
    TEMP_SOURCE_INTERNAL = 0,   /* wired NTC probe on ADC1 ch6            */
    TEMP_SOURCE_PILL,           /* RAPT Pill / RAPT Temp over BLE         */
} temp_source_t;

/*
 * Naming follows the stock firmware's own menu vocabulary - "Always off",
 * "Automatic", "Always on" - so the panel, the web UI and an original unit
 * all describe the same three behaviours the same way.
 */
typedef enum {
    FAN_MODE_ALWAYS_OFF = 0,    /* never run the fan                       */
    FAN_MODE_AUTOMATIC,         /* run while heating or cooling            */
    FAN_MODE_ALWAYS_ON,         /* run continuously - even circulation     */
} fan_mode_t;

typedef enum {
    THERMO_IDLE = 0,
    THERMO_COOLING,
    THERMO_HEATING,
    THERMO_DEHUMIDIFYING,       /* reheat: compressor + heater together    */
    THERMO_DEHUM_RESTING,       /* duty cap hit; cooling off for a while   */
    THERMO_FAULT,               /* no usable temperature; outputs safe     */
} thermo_state_t;

typedef struct {
    bool          enabled;
    float         target_c;
    float         cooler_hys;      /* stock default 1.0 */
    float         heater_hys;      /* stock default 1.0 */
    temp_source_t source;
    char          pill_addr[18];   /* empty = most recently heard sensor */
    fan_mode_t    fan_mode;
    /*
     * Pill staleness. The Pill's broadcast interval is configurable in the
     * RAPT portal and is often 5 minutes, so this MUST be a multiple of that
     * interval, not equal to it - otherwise a single missed advertisement
     * faults the controller. Default 30 min tolerates several misses.
     *
     * Controlling on a Pill means controlling WORT temperature, which has
     * enormous thermal mass and moves slowly. A reading a few minutes old is
     * perfectly good control input; the same would not be true of chamber air.
     */
    uint32_t      sensor_timeout_s;

    /*
     * Humidity staleness - deliberately separate and much shorter. The SHT is
     * a local I2C part polled every 5 s, so anything beyond a minute means it
     * has failed. Sharing the Pill's timeout here would let a dead sensor go
     * unnoticed for half an hour while reheat runs.
     */
    uint32_t      rh_timeout_s;

    float         min_temp;        /* refuse targets outside these       */
    float         max_temp;

    /*
     * Reheat dehumidification. Runs the compressor to drop the evaporator
     * below dew point while the heater cancels the sensible cooling, so
     * moisture leaves without the temperature moving.
     *
     * Only appropriate on a chamber whose evaporator DRAINS. If the coil
     * merely frosts, the water comes back at the next defrost.
     */
    /* Alarm thresholds travel with the control config; see alarms.h. */
    alarms_config_t alarms;

    bool          humidity_enabled;
    float         rh_target;        /* percent */
    float         rh_hys;           /* start above target+hys, stop at target */
    float         reheat_max_dev_c; /* abort if temp strays this far          */
    uint32_t      reheat_max_min;   /* max continuous run                     */
    uint32_t      reheat_rest_min;  /* forced rest after hitting the cap      */
} thermostat_config_t;

#define THERMOSTAT_DEFAULT_CONFIG() ((thermostat_config_t){ \
    .enabled          = false,                              \
    .target_c         = 20.0f,                              \
    .cooler_hys       = 1.0f,                               \
    .heater_hys       = 1.0f,                               \
    .source           = TEMP_SOURCE_INTERNAL,               \
    .pill_addr        = "",                                 \
    .fan_mode         = FAN_MODE_AUTOMATIC,                 \
    .sensor_timeout_s = 1800,                               \
    .rh_timeout_s     = 60,                                 \
    .min_temp         = -10.0f,                             \
    .max_temp         = 50.0f,                              \
    .alarms           = ALARMS_DEFAULT_CONFIG(),            \
    .humidity_enabled = false,                              \
    .rh_target        = 80.0f,                              \
    .rh_hys           = 3.0f,                               \
    .reheat_max_dev_c = 2.0f,                               \
    .reheat_max_min   = 30,                                 \
    .reheat_rest_min  = 15,                                 \
})

typedef struct {
    thermo_state_t state;
    bool           temp_valid;
    float          temp_c;         /* the value control is acting on */
    const char    *source_name;
    const char    *fault;          /* NULL when healthy */
    uint32_t       state_seconds;  /* time in the current state */
    bool           rh_valid;
    float          rh;
    bool           reheat_active;
    uint32_t       reheat_seconds; /* time in the current reheat run */
    uint32_t       rest_remaining_s;
} thermostat_status_t;

esp_err_t thermostat_start(const thermostat_config_t *cfg);

void thermostat_get_config(thermostat_config_t *out);
esp_err_t thermostat_set_config(const thermostat_config_t *cfg);

void thermostat_get_status(thermostat_status_t *out);

/* Persist / restore the configuration across reboots. */
esp_err_t thermostat_save(void);
esp_err_t thermostat_load(thermostat_config_t *out);

const char *thermostat_state_str(thermo_state_t s);
const char *thermostat_fan_mode_str(fan_mode_t m);
const char *thermostat_source_str(temp_source_t s);
