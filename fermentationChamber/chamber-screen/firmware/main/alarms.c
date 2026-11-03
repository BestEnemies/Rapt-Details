#include "alarms.h"
#include "relays.h"

#include <string.h>
#include <math.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "alarms";

static alarm_state_t    s_st[ALARM_COUNT];
static alarms_config_t  s_cfg;
static SemaphoreHandle_t s_lock;

/* Condition timers - a condition must persist before it becomes an alarm. */
static int64_t s_door_since_us;
static int64_t s_off_target_since_us;

/* Buzzer pattern state. */
static int64_t s_beep_until_us;
static int64_t s_next_beep_us;

/* Stock firmware wording. */
static const char *TEXT[ALARM_COUNT] = {
    [ALARM_PROBE_FAULT] = "Temperature probe disconnected or shorted",
    [ALARM_TEMP_OVER]   = "Temperature over limits",
    [ALARM_TEMP_UNDER]  = "Temperature below limits",
    [ALARM_UNREACHABLE] = "Target temperature unreachable",
    [ALARM_DOOR_OPEN]   = "Door has been left open",
    [ALARM_SENSOR_DIFF] = "Temperature sensors disagree",
    [ALARM_SENSOR_LOST] = "Temperature reading error",
};

static const char *KEY[ALARM_COUNT] = {
    [ALARM_PROBE_FAULT] = "probe",
    [ALARM_TEMP_OVER]   = "over",
    [ALARM_TEMP_UNDER]  = "under",
    [ALARM_UNREACHABLE] = "unreachable",
    [ALARM_DOOR_OPEN]   = "door",
    [ALARM_SENSOR_DIFF] = "sensor_diff",
    [ALARM_SENSOR_LOST] = "sensor_lost",
};

/*
 * Severity order, most serious first. Anything that means we are no longer
 * measuring reliably outranks anything derived FROM a measurement - a bad
 * reading can manufacture a plausible-looking over-temperature alarm, so the
 * sensor fault is the one worth showing.
 */
static const alarm_id_t SEVERITY[ALARM_COUNT] = {
    ALARM_PROBE_FAULT,
    ALARM_SENSOR_LOST,
    ALARM_SENSOR_DIFF,
    ALARM_TEMP_OVER,
    ALARM_TEMP_UNDER,
    ALARM_UNREACHABLE,
    ALARM_DOOR_OPEN,
};

const char *alarms_text(alarm_id_t id)
{
    return (id < ALARM_COUNT && TEXT[id]) ? TEXT[id] : "?";
}

const char *alarms_key(alarm_id_t id)
{
    return (id < ALARM_COUNT && KEY[id]) ? KEY[id] : "?";
}

/* ------------------------------------------------------------------ */

static void set_condition(alarm_id_t id, bool now_true)
{
    alarm_state_t *a = &s_st[id];
    a->active = now_true;

    if (now_true && !a->latched) {
        a->latched   = true;
        a->silenced  = false;
        a->raised_us = esp_timer_get_time();
        a->count++;
        ESP_LOGW(TAG, "RAISED: %s", alarms_text(id));

        if (s_cfg.sounds_enabled) {
            /* Beep for 30 s or until silenced, whichever comes first. */
            s_beep_until_us = a->raised_us + 30000000LL;
            s_next_beep_us  = 0;
        }
    }
}

static void drive_buzzer(void)
{
    int64_t now = esp_timer_get_time();

    /* Any un-silenced, still-active alarm keeps the pattern going. */
    bool want = false;
    for (int i = 0; i < ALARM_COUNT; i++) {
        if (s_st[i].latched && s_st[i].active && !s_st[i].silenced) {
            want = true;
            break;
        }
    }

    if (!want || !s_cfg.sounds_enabled || now > s_beep_until_us) {
        relays_set(RELAY_BUZZER, false);
        return;
    }

    /* 200 ms on, 1800 ms off - audible without being unbearable. */
    if (now >= s_next_beep_us) {
        bool on = !relays_get(RELAY_BUZZER);
        relays_set(RELAY_BUZZER, on);
        s_next_beep_us = now + (on ? 200000LL : 1800000LL);
    }
}

void alarms_evaluate(const alarm_inputs_t *in)
{
    if (!in) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);

    int64_t now = esp_timer_get_time();

    /* --- sensing faults ------------------------------------------- */
    set_condition(ALARM_PROBE_FAULT, in->probe_fault);

    /*
     * "No controlling temperature" is only a fault if we are supposed to be
     * controlling. With the thermostat disabled there is no reading by
     * definition, and raising a sensor alarm for that is a false alarm - the
     * fastest way to teach someone to ignore the alarm panel entirely.
     */
    set_condition(ALARM_SENSOR_LOST,
                  in->controlling && !in->temp_valid && !in->probe_fault);

    /*
     * Cross-check. Only meaningful with two readings, and only if neither is
     * already known bad - otherwise this fires as a duplicate of the fault
     * that caused the disagreement.
     */
    bool diff = false;
    if (in->temp_valid && in->second_valid && !in->probe_fault) {
        diff = fabsf(in->temp_c - in->second_c) > s_cfg.max_sensor_diff_c;
    }
    set_condition(ALARM_SENSOR_DIFF, diff);

    /* --- range, but only when the reading is trustworthy ----------- */
    if (in->temp_valid && !in->probe_fault) {
        set_condition(ALARM_TEMP_OVER,  in->temp_c > s_cfg.alarm_high_c);
        set_condition(ALARM_TEMP_UNDER, in->temp_c < s_cfg.alarm_low_c);
    } else {
        s_st[ALARM_TEMP_OVER].active  = false;
        s_st[ALARM_TEMP_UNDER].active = false;
    }

    /* --- door held open -------------------------------------------- */
    if (in->door_open) {
        if (!s_door_since_us) {
            s_door_since_us = now;
        }
        set_condition(ALARM_DOOR_OPEN,
                      (now - s_door_since_us) > (int64_t)s_cfg.door_open_s * 1000000LL);
    } else {
        s_door_since_us = 0;
        set_condition(ALARM_DOOR_OPEN, false);
    }

    /*
     * --- target unreachable ----------------------------------------
     * Only counts while we are actually trying. Sitting far from target with
     * the outputs idle is not a fault - it is the thermostat being disabled,
     * or the deadband doing its job.
     */
    bool far_off = in->temp_valid && !in->probe_fault && in->working &&
                   fabsf(in->temp_c - in->target_c) > s_cfg.unreachable_dev_c;
    if (far_off) {
        if (!s_off_target_since_us) {
            s_off_target_since_us = now;
        }
        set_condition(ALARM_UNREACHABLE,
                      (now - s_off_target_since_us) > (int64_t)s_cfg.unreachable_s * 1000000LL);
    } else {
        s_off_target_since_us = 0;
        set_condition(ALARM_UNREACHABLE, false);
    }

    drive_buzzer();
    xSemaphoreGive(s_lock);
}

/* ------------------------------------------------------------------ */

esp_err_t alarms_init(const alarms_config_t *cfg)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    s_cfg = cfg ? *cfg : (alarms_config_t)ALARMS_DEFAULT_CONFIG();
    memset(s_st, 0, sizeof(s_st));
    ESP_LOGI(TAG, "init: limits %.1f..%.1fC, sensor diff %.1fC, door %us, sounds %s",
             s_cfg.alarm_low_c, s_cfg.alarm_high_c, s_cfg.max_sensor_diff_c,
             (unsigned)s_cfg.door_open_s, s_cfg.sounds_enabled ? "on" : "off");
    return ESP_OK;
}

void alarms_configure(const alarms_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg = *cfg;
    xSemaphoreGive(s_lock);
}

void alarms_get_config(alarms_config_t *out)
{
    if (out) {
        *out = s_cfg;
    }
}

bool alarms_get(alarm_id_t id, alarm_state_t *out)
{
    if (id >= ALARM_COUNT || !out) {
        return false;
    }
    *out = s_st[id];
    return true;
}

bool alarms_highest(alarm_id_t *id)
{
    for (int i = 0; i < ALARM_COUNT; i++) {
        alarm_id_t a = SEVERITY[i];
        if (s_st[a].latched) {
            if (id) {
                *id = a;
            }
            return true;
        }
    }
    return false;
}

bool alarms_any(void)
{
    return alarms_highest(NULL);
}

void alarms_silence(alarm_id_t id)
{
    if (id < ALARM_COUNT && s_st[id].latched) {
        s_st[id].silenced = true;
        ESP_LOGI(TAG, "silenced: %s", alarms_text(id));
        relays_set(RELAY_BUZZER, false);
    }
}

void alarms_dismiss(alarm_id_t id)
{
    if (id >= ALARM_COUNT || !s_st[id].latched) {
        return;
    }
    if (s_st[id].active) {
        /*
         * Refuse rather than silently re-raising on the next tick: an alarm
         * that will not go away is telling you something, and pretending the
         * button worked is worse than saying it did not.
         */
        ESP_LOGW(TAG, "cannot dismiss %s - condition is still true",
                 alarms_text(id));
        s_st[id].silenced = true;
        relays_set(RELAY_BUZZER, false);
        return;
    }
    s_st[id].latched  = false;
    s_st[id].silenced = false;
    ESP_LOGI(TAG, "cleared: %s", alarms_text(id));
}

void alarms_silence_all(void)
{
    for (int i = 0; i < ALARM_COUNT; i++) {
        alarms_silence((alarm_id_t)i);
    }
}

void alarms_dismiss_all(void)
{
    for (int i = 0; i < ALARM_COUNT; i++) {
        alarms_dismiss((alarm_id_t)i);
    }
}
