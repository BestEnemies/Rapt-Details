#include "thermostat.h"
#include "relays.h"
#include "temp_probe.h"
#include "pill.h"
#include "humidity.h"
#include "alarms.h"
#include "buttons.h"

#include <string.h>
#include <math.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "thermostat";

#define NVS_NAMESPACE  "chamber"
#define TICK_MS        5000

static thermostat_config_t s_cfg;
static thermostat_status_t s_status;
static SemaphoreHandle_t   s_lock;
static int64_t             s_state_since_us;
static int64_t             s_reheat_since_us;  /* 0 = not currently reheating */
static int64_t             s_rest_until_us;    /* forced rest after duty cap  */

const char *thermostat_state_str(thermo_state_t s)
{
    switch (s) {
    case THERMO_IDLE:           return "idle";
    case THERMO_COOLING:        return "cooling";
    case THERMO_HEATING:        return "heating";
    case THERMO_DEHUMIDIFYING:  return "dehumidifying";
    case THERMO_DEHUM_RESTING:  return "dehum_resting";
    case THERMO_FAULT:          return "fault";
    default:                    return "?";
    }
}

const char *thermostat_fan_mode_str(fan_mode_t m)
{
    switch (m) {
    case FAN_MODE_ALWAYS_OFF: return "always_off";
    case FAN_MODE_AUTOMATIC:  return "automatic";
    case FAN_MODE_ALWAYS_ON:  return "always_on";
    default:                   return "?";
    }
}

const char *thermostat_source_str(temp_source_t s)
{
    return (s == TEMP_SOURCE_PILL) ? "pill" : "internal";
}

/* ------------------------------------------------------------------ */

static void set_state(thermo_state_t st)
{
    if (s_status.state != st) {
        ESP_LOGI(TAG, "%s -> %s",
                 thermostat_state_str(s_status.state),
                 thermostat_state_str(st));
        s_status.state = st;
        s_state_since_us = esp_timer_get_time();
    }
}

/*
 * Fetch the controlling temperature. Returns false and sets *fault when there
 * is nothing trustworthy to act on - a stale Pill counts as no reading, which
 * matters because a fridge left cooling against a frozen sensor value is how
 * you end up with a frozen ferment.
 */
static bool read_temperature(float *out, const char **fault)
{
    if (s_cfg.source == TEMP_SOURCE_PILL) {
        pill_reading_t p;
        if (!pill_get_by_addr(s_cfg.pill_addr, &p)) {
            *fault = "no pill data";
            return false;
        }
        int age = (int)((esp_timer_get_time() - p.last_seen_us) / 1000000);
        if (age > (int)s_cfg.sensor_timeout_s) {
            *fault = "pill stale";
            return false;
        }
        /*
         * Once the real broadcast period is known, complain if the timeout is
         * too tight to survive a missed advertisement. A Pill set to 5-minute
         * transmissions with a 5-minute timeout faults on any jitter at all.
         */
        if (p.interval_s > 0 &&
            s_cfg.sensor_timeout_s < (uint32_t)(p.interval_s * 5 / 2)) {
            ESP_LOGW(TAG, "sensor_timeout %us is tight for a %ds broadcast "
                          "period - a single missed advert will fault",
                     (unsigned)s_cfg.sensor_timeout_s, p.interval_s);
        }
        *out = p.celsius;
        return true;
    }

    probe_reading_t r;
    if (temp_probe_read(&r) != ESP_OK) {
        *fault = "probe read failed";
        return false;
    }
    if (r.status != PROBE_OK) {
        *fault = temp_probe_status_str(r.status);
        return false;
    }
    if (isnan(r.celsius)) {
        *fault = "probe nan";
        return false;
    }
    *out = r.celsius;
    return true;
}

static void apply_fan(bool active)
{
    switch (s_cfg.fan_mode) {
    case FAN_MODE_ALWAYS_ON:
        relays_set(RELAY_FAN, true);
        break;
    case FAN_MODE_AUTOMATIC:
        relays_set(RELAY_FAN, active);
        break;
    case FAN_MODE_ALWAYS_OFF:
    default:
        relays_set(RELAY_FAN, false);
        break;
    }
}

/*
 * Decide whether reheat dehumidification should be running.
 *
 * Every one of these conditions is a safety gate, not a preference. Running
 * the compressor and the heater against each other means a stuck sensor stops
 * being an inconvenience and starts being able to cook or freeze the chamber,
 * so the mode is only entered while everything is demonstrably healthy and it
 * drops out the moment anything is not.
 */
static bool want_reheat(const thermostat_config_t *cfg, float t, bool t_valid)
{
    if (!cfg->humidity_enabled || !t_valid) {
        return false;
    }

    int64_t now = esp_timer_get_time();

    /* Serving out a forced rest after hitting the duty cap. */
    if (now < s_rest_until_us) {
        return false;
    }

    humidity_reading_t h;
    if (!humidity_get(&h)) {
        return false;                       /* no sensor, no reheat */
    }
    /*
     * Deliberately NOT sensor_timeout_s - that one is sized for a Pill that
     * may only broadcast every few minutes. The SHT is polled every 5 s over
     * I2C, so anything beyond rh_timeout_s means it has failed, and reheat
     * must not keep running the compressor and heater against each other on
     * the strength of a stale humidity value.
     */
    int age = (int)((now - h.last_us) / 1000000);
    if (age > (int)cfg->rh_timeout_s) {
        ESP_LOGW(TAG, "humidity stale (%ds > %us); reheat withheld",
                 age, (unsigned)cfg->rh_timeout_s);
        return false;
    }

    /* Temperature must be under control before we start fighting ourselves. */
    if (fabsf(t - cfg->target_c) > cfg->reheat_max_dev_c) {
        if (s_reheat_since_us) {
            ESP_LOGW(TAG, "temp %.2f is %.2f from target; aborting reheat",
                     t, fabsf(t - cfg->target_c));
        }
        return false;
    }

    /* Duty cap: long continuous reheat frosts the coil and wastes energy. */
    if (s_reheat_since_us &&
        (now - s_reheat_since_us) > (int64_t)cfg->reheat_max_min * 60000000LL) {
        ESP_LOGI(TAG, "reheat duty cap reached; resting %u min",
                 (unsigned)cfg->reheat_rest_min);
        s_rest_until_us = now + (int64_t)cfg->reheat_rest_min * 60000000LL;
        return false;
    }

    /* Hysteresis: start above target+hys, continue until back at target. */
    if (s_reheat_since_us) {
        return h.rh > cfg->rh_target;
    }
    return h.rh > cfg->rh_target + cfg->rh_hys;
}


/*
 * Feed the alarm evaluator. The cross-check compares whichever sensor is in
 * control against the other one - that is the pairing that catches a probe
 * drifting or a Pill reporting a different vessel entirely.
 */
static void feed_alarms(const thermostat_config_t *cfg,
                        bool temp_valid, float t, bool working)
{
    alarm_inputs_t in = {
        .temp_valid  = temp_valid,
        .temp_c      = t,
        .working     = working,
        .controlling = cfg->enabled,
        .target_c    = cfg->target_c,
        .door_open   = buttons_door_is_open(),
    };

    /* One read: each costs 128 ADC samples. */
    probe_reading_t pr;
    bool read_ok   = (temp_probe_read(&pr) == ESP_OK);
    bool probe_ok  = read_ok && (pr.status == PROBE_OK);
    in.probe_fault = read_ok && (pr.status != PROBE_OK);

    /*
     * With control off there is no "controlling" reading, but the chamber can
     * still be dangerously hot or cold - so fall back to the probe so the
     * range alarms keep working.
     */
    if (!temp_valid && probe_ok && !isnan(pr.celsius)) {
        in.temp_valid = true;
        in.temp_c     = pr.celsius;
    }

    if (cfg->source == TEMP_SOURCE_PILL) {
        in.second_valid = probe_ok && !isnan(pr.celsius);
        in.second_c     = pr.celsius;
    } else {
        pill_reading_t p;
        if (pill_get_by_addr(cfg->pill_addr, &p)) {
            int age = (int)((esp_timer_get_time() - p.last_seen_us) / 1000000);
            in.second_valid = age <= (int)cfg->sensor_timeout_s;
            in.second_c     = p.celsius;
        }
    }

    alarms_evaluate(&in);
}

static void control_tick(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    thermostat_config_t cfg = s_cfg;
    xSemaphoreGive(s_lock);

    if (!cfg.enabled) {
        relays_allow_simultaneous(false);
        s_reheat_since_us = 0;
        relays_request_cool(false);
        relays_request_heat(false);
        set_state(THERMO_IDLE);
        s_status.temp_valid = false;
        s_status.fault = NULL;
        /*
         * Fan still honours its mode when the thermostat is off - "always on"
         * means always on, which is the point of the setting.
         */
        apply_fan(false);
        feed_alarms(&cfg, false, 0.0f, false);
        return;
    }

    const char *fault = NULL;
    float t = 0.0f;
    if (!read_temperature(&t, &fault)) {
        /*
         * Nothing trustworthy: de-energise both and say why. emergency_stop
         * also revokes the reheat permission, which is exactly what we want
         * when the sensor is the thing that failed.
         */
        relays_emergency_stop();
        apply_fan(false);
        s_reheat_since_us = 0;
        s_status.temp_valid = false;
        s_status.fault = fault;
        set_state(THERMO_FAULT);
        feed_alarms(&cfg, false, 0.0f, false);
        return;
    }

    s_status.temp_valid = true;
    s_status.temp_c     = t;
    s_status.fault      = NULL;

    humidity_reading_t hr;
    s_status.rh_valid = humidity_get(&hr);
    if (s_status.rh_valid) {
        s_status.rh = hr.rh;
    }

    /* ---- reheat dehumidification takes priority when it is warranted ---- */
    if (want_reheat(&cfg, t, true)) {
        if (!s_reheat_since_us) {
            s_reheat_since_us = esp_timer_get_time();
            ESP_LOGI(TAG, "reheat dehumidify: RH %.1f%% > %.1f%%, holding %.1fC",
                     s_status.rh, cfg.rh_target, cfg.target_c);
        }
        relays_allow_simultaneous(true);

        /* Compressor runs to condense; heater bang-bangs to hold temperature. */
        relays_request_cool(true);
        relays_request_heat(t < cfg.target_c);

        /* Circulation is not optional here - it drives both effects. */
        relays_set(RELAY_FAN, true);

        s_status.reheat_active = true;
        set_state(THERMO_DEHUMIDIFYING);
        feed_alarms(&cfg, true, t, true);
        return;
    }

    if (s_reheat_since_us) {
        ESP_LOGI(TAG, "reheat dehumidify ended after %ds",
                 (int)((esp_timer_get_time() - s_reheat_since_us) / 1000000));
        s_reheat_since_us = 0;
    }
    relays_allow_simultaneous(false);
    s_status.reheat_active = false;

    bool cooling = relays_get(RELAY_COOLER);
    bool heating = relays_get(RELAY_HEATER);

    bool want_cool = cooling;
    bool want_heat = heating;

    /* Cut-in above target+hys, cut-out back at target. */
    if (!cooling && t > cfg.target_c + cfg.cooler_hys) {
        want_cool = true;
    } else if (cooling && t <= cfg.target_c) {
        want_cool = false;
    }

    if (!heating && t < cfg.target_c - cfg.heater_hys) {
        want_heat = true;
    } else if (heating && t >= cfg.target_c) {
        want_heat = false;
    }

    /* Belt and braces - the relay layer enforces this too. */
    if (want_cool && want_heat) {
        want_cool = want_heat = false;
    }

    esp_err_t ce = relays_request_cool(want_cool);
    esp_err_t he = relays_request_heat(want_heat);

    if (ce == ESP_ERR_INVALID_STATE) {
        ESP_LOGD(TAG, "cool %s deferred, %us of protection left",
                 want_cool ? "start" : "stop",
                 (unsigned)relays_cool_blocked_for());
    }
    if (he == ESP_ERR_INVALID_STATE) {
        ESP_LOGD(TAG, "heat %s deferred, %us of protection left",
                 want_heat ? "start" : "stop",
                 (unsigned)relays_heat_blocked_for());
    }

    bool active = relays_get(RELAY_COOLER) || relays_get(RELAY_HEATER);
    apply_fan(active);

    if (relays_get(RELAY_COOLER)) {
        set_state(THERMO_COOLING);
    } else if (relays_get(RELAY_HEATER)) {
        set_state(THERMO_HEATING);
    } else if (esp_timer_get_time() < s_rest_until_us) {
        /*
         * Idle, but serving out the post-duty-cap rest. Reported distinctly
         * so "why is it not dehumidifying?" has a visible answer rather than
         * looking like the humidistat has simply stopped working.
         */
        set_state(THERMO_DEHUM_RESTING);
    } else {
        set_state(THERMO_IDLE);
    }

    feed_alarms(&cfg, true, t,
                relays_get(RELAY_COOLER) || relays_get(RELAY_HEATER));
}

static void thermostat_task(void *arg)
{
    (void)arg;
    for (;;) {
        control_tick();
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

/* ------------------------------------------------------------------ */

esp_err_t thermostat_start(const thermostat_config_t *cfg)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    s_cfg = cfg ? *cfg : (thermostat_config_t)THERMOSTAT_DEFAULT_CONFIG();
    s_status.state       = THERMO_IDLE;
    s_status.source_name = thermostat_source_str(s_cfg.source);
    s_state_since_us     = esp_timer_get_time();

    ESP_ERROR_CHECK(alarms_init(&s_cfg.alarms));

    ESP_LOGI(TAG, "start: %s, target %.1fC, hys -%.1f/+%.1f, source %s, fan %s",
             s_cfg.enabled ? "enabled" : "disabled",
             s_cfg.target_c, s_cfg.heater_hys, s_cfg.cooler_hys,
             thermostat_source_str(s_cfg.source),
             thermostat_fan_mode_str(s_cfg.fan_mode));

    if (xTaskCreate(thermostat_task, "thermostat", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void thermostat_get_config(thermostat_config_t *out)
{
    if (!out) {
        return;
    }
    if (!s_lock) {
        /* Called before thermostat_start(): hand back defaults, do not crash. */
        *out = (thermostat_config_t)THERMOSTAT_DEFAULT_CONFIG();
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_cfg;
    xSemaphoreGive(s_lock);
}

esp_err_t thermostat_set_config(const thermostat_config_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->target_c < cfg->min_temp || cfg->target_c > cfg->max_temp) {
        ESP_LOGW(TAG, "target %.1f outside %.1f..%.1f, refusing",
                 cfg->target_c, cfg->min_temp, cfg->max_temp);
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->cooler_hys <= 0.0f || cfg->heater_hys <= 0.0f) {
        ESP_LOGW(TAG, "hysteresis must be positive, refusing");
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg = *cfg;
    s_status.source_name = thermostat_source_str(s_cfg.source);
    xSemaphoreGive(s_lock);

    alarms_configure(&cfg->alarms);

    ESP_LOGI(TAG, "config: %s, target %.1fC, hys -%.1f/+%.1f, source %s, fan %s",
             cfg->enabled ? "enabled" : "disabled",
             cfg->target_c, cfg->heater_hys, cfg->cooler_hys,
             thermostat_source_str(cfg->source),
             thermostat_fan_mode_str(cfg->fan_mode));

    /* Act on the change now rather than waiting for the next tick. */
    control_tick();
    return ESP_OK;
}

void thermostat_get_status(thermostat_status_t *out)
{
    if (!out) {
        return;
    }
    if (!s_lock) {
        memset(out, 0, sizeof(*out));
        out->source_name = "internal";
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int64_t now = esp_timer_get_time();
    *out = s_status;
    out->source_name   = thermostat_source_str(s_cfg.source);
    out->state_seconds = (uint32_t)((now - s_state_since_us) / 1000000);
    out->reheat_seconds = s_reheat_since_us
                        ? (uint32_t)((now - s_reheat_since_us) / 1000000) : 0;
    out->rest_remaining_s = (now < s_rest_until_us)
                        ? (uint32_t)((s_rest_until_us - now) / 1000000) : 0;
    xSemaphoreGive(s_lock);
}

/* ------------------------------------------------------------------ */
/* persistence                                                          */

/*
 * Bump whenever thermostat_config_t changes shape or a field's meaning
 * changes. A stored blob whose version does not match is discarded rather
 * than reinterpreted - silently reading old bytes into new fields produced a
 * humidity timeout of 3240099840 seconds, which is exactly the kind of value
 * that then gets used as a safety limit.
 */
#define CONFIG_VERSION  3

esp_err_t thermostat_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    thermostat_config_t c;
    thermostat_get_config(&c);

    err = nvs_set_blob(h, "thermo", &c, sizeof(c));
    if (err == ESP_OK) {
        err = nvs_set_u32(h, "thermo_ver", CONFIG_VERSION);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "config saved (v%d)", CONFIG_VERSION);
    }
    return err;
}

esp_err_t thermostat_load(thermostat_config_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }

    uint32_t ver = 0;
    err = nvs_get_u32(h, "thermo_ver", &ver);
    if (err != ESP_OK || ver != CONFIG_VERSION) {
        nvs_close(h);
        ESP_LOGW(TAG, "stored config is v%lu, this build wants v%d; using defaults",
                 (unsigned long)ver, CONFIG_VERSION);
        return ESP_ERR_INVALID_VERSION;
    }

    /*
     * Load into a scratch struct, never the caller's. nvs_get_blob copies
     * whatever is stored before any validation we do, so handing it `out`
     * directly corrupts the caller's defaults even when we then reject it.
     */
    thermostat_config_t tmp;
    size_t len = sizeof(tmp);
    err = nvs_get_blob(h, "thermo", &tmp, &len);
    nvs_close(h);

    if (err != ESP_OK) {
        return err;
    }
    if (len != sizeof(tmp)) {
        ESP_LOGW(TAG, "stored config is %u bytes, expected %u; using defaults",
                 (unsigned)len, (unsigned)sizeof(tmp));
        return ESP_ERR_INVALID_SIZE;
    }

    *out = tmp;
    return ESP_OK;
}
