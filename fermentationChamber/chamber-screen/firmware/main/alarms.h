/*
 * alarms.h - fault and out-of-range detection.
 *
 * The stock firmware carries nine alarm strings; these are the ones that can
 * be evaluated from the hardware we have, using its wording so the device
 * reads the same to anyone who has used an original unit.
 *
 * Two principles the implementation follows:
 *
 * An alarm is a LATCH, not a level. Once raised it stays raised until the
 * condition clears AND someone acknowledges it, because the whole point is to
 * survive being unobserved - a door left open for an hour overnight must
 * still be visible in the morning even though it is shut by then.
 *
 * Silencing is not dismissing. Silence stops the buzzer and leaves the alarm
 * showing; dismiss clears it. Conflating the two means the only way to stop
 * the noise is to destroy the evidence.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    ALARM_PROBE_FAULT = 0,   /* "Temperature probe disconnected or shorted" */
    ALARM_TEMP_OVER,         /* "Temperature over limits"                   */
    ALARM_TEMP_UNDER,        /* "Temperature below limits"                  */
    ALARM_UNREACHABLE,       /* "Target temperature unreachable"            */
    ALARM_DOOR_OPEN,         /* "Door has been left open"                   */
    ALARM_SENSOR_DIFF,       /* sensors disagree by more than allowed       */
    ALARM_SENSOR_LOST,       /* the controlling sensor went quiet           */
    ALARM_COUNT
} alarm_id_t;

typedef struct {
    bool     active;        /* condition is true right now       */
    bool     latched;       /* has been true and not yet cleared */
    bool     silenced;      /* buzzer suppressed, still showing  */
    int64_t  raised_us;
    uint32_t count;         /* times raised since boot           */
} alarm_state_t;

typedef struct {
    float    alarm_low_c;        /* below this -> TEMP_UNDER      */
    float    alarm_high_c;       /* above this -> TEMP_OVER       */
    float    max_sensor_diff_c;  /* stock "Allowed sensor diff."  */
    uint32_t door_open_s;        /* door held open this long      */
    uint32_t unreachable_s;      /* off target this long while working */
    float    unreachable_dev_c;  /* ...by at least this much      */
    bool     sounds_enabled;
} alarms_config_t;

#define ALARMS_DEFAULT_CONFIG() ((alarms_config_t){ \
    .alarm_low_c       = 0.0f,                      \
    .alarm_high_c      = 40.0f,                     \
    .max_sensor_diff_c = 5.0f,                      \
    .door_open_s       = 300,                       \
    .unreachable_s     = 7200,                      \
    .unreachable_dev_c = 3.0f,                      \
    .sounds_enabled    = true,                      \
})

/* What the evaluator needs to know. Absent readings are simply flagged. */
typedef struct {
    bool  temp_valid;
    float temp_c;              /* the controlling temperature      */
    bool  probe_fault;
    bool  second_valid;        /* the other sensor, for cross-check */
    float second_c;
    bool  door_open;
    bool  working;             /* heating or cooling right now      */
    bool  controlling;         /* thermostat enabled and acting     */
    float target_c;
} alarm_inputs_t;

esp_err_t alarms_init(const alarms_config_t *cfg);
void alarms_configure(const alarms_config_t *cfg);
void alarms_get_config(alarms_config_t *out);

/* Call once per control tick. Drives latching and the buzzer. */
void alarms_evaluate(const alarm_inputs_t *in);

bool alarms_get(alarm_id_t id, alarm_state_t *out);

/* The alarm worth showing: highest severity, most recent as a tiebreak. */
bool alarms_highest(alarm_id_t *id);

/* Any latched alarm at all. */
bool alarms_any(void);

void alarms_silence(alarm_id_t id);   /* stop the noise, keep the alarm */
void alarms_dismiss(alarm_id_t id);   /* clear it, if the condition has gone */
void alarms_silence_all(void);
void alarms_dismiss_all(void);

const char *alarms_text(alarm_id_t id);   /* stock wording */
const char *alarms_key(alarm_id_t id);    /* short id for JSON */
