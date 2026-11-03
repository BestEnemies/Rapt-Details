#include "webui.h"
#include "board.h"
#include "netlog.h"
#include "wifi.h"
#include "relays.h"
#include "buttons.h"
#include "temp_probe.h"
#include "pill.h"
#include "thermostat.h"
#include "humidity.h"
#include "alarms.h"
#include "ui.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "webui";

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[]   asm("_binary_index_html_end");

static httpd_handle_t s_server;

/* ------------------------------------------------------------------ */

static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, index_html_start,
                           index_html_end - index_html_start - 1);
}

static esp_err_t status_get(httpd_req_t *req)
{
    probe_reading_t p = { 0 };
    temp_probe_read(&p);

    const esp_app_desc_t *desc = esp_app_get_description();
    const esp_partition_t *run = esp_ota_get_running_partition();

    probe_cal_t pc;
    temp_probe_get_cal(&pc);

    char probe[320];
    if (p.status == PROBE_OK) {
        snprintf(probe, sizeof(probe),
                 "\"ok\":true,\"raw\":%d,\"mv\":%d,\"ohms\":%.1f,"
                 "\"celsius\":%.2f,\"celsius_raw\":%.2f,"
                 "\"cal\":{\"mode\":\"%s\",\"p0\":%s,\"p1\":%s,"
                 "\"m0\":%.2f,\"a0\":%.2f,\"m1\":%.2f,\"a1\":%.2f,\"offset\":%.2f}",
                 p.raw_mean, p.millivolts, p.resistance,
                 p.celsius, p.celsius_raw,
                 pc.two_point ? "two_point" : (pc.have0 ? "one_point" :
                     (pc.offset != 0.0f ? "offset" : "none")),
                 pc.have0 ? "true" : "false", pc.have1 ? "true" : "false",
                 pc.measured0, pc.actual0, pc.measured1, pc.actual1, pc.offset);
    } else {
        snprintf(probe, sizeof(probe),
                 "\"ok\":false,\"fault\":\"%s\",\"raw\":%d",
                 temp_probe_status_str(p.status), p.raw_mean);
    }

    /* thermostat */
    thermostat_config_t tc;
    thermostat_status_t ts;
    thermostat_get_config(&tc);
    thermostat_get_status(&ts);

    char thermo[560];
    snprintf(thermo, sizeof(thermo),
        "\"enabled\":%s,\"state\":\"%s\",\"state_s\":%lu,"
        "\"target\":%.2f,\"cooler_hys\":%.2f,\"heater_hys\":%.2f,"
        "\"source\":\"%s\",\"fan_mode\":\"%s\",\"pill_addr\":\"%s\","
        "\"temp_valid\":%s,\"temp\":%.2f,\"fault\":%s%s%s,"
        "\"humidity_enabled\":%s,\"rh_valid\":%s,\"rh\":%.1f,"
        "\"rh_target\":%.1f,\"rh_hys\":%.1f,\"hum_sensor\":\"%s\","
        "\"reheat\":%s,\"reheat_s\":%lu,\"rest_s\":%lu,"
        "\"reheat_max_dev\":%.1f,\"reheat_max_min\":%lu,"
        "\"sensor_timeout\":%lu,\"rh_timeout\":%lu",
        tc.enabled ? "true" : "false",
        thermostat_state_str(ts.state), (unsigned long)ts.state_seconds,
        tc.target_c, tc.cooler_hys, tc.heater_hys,
        thermostat_source_str(tc.source),
        thermostat_fan_mode_str(tc.fan_mode),
        tc.pill_addr,
        ts.temp_valid ? "true" : "false",
        ts.temp_valid ? ts.temp_c : 0.0f,
        ts.fault ? "\"" : "null", ts.fault ? ts.fault : "", ts.fault ? "\"" : "",
        tc.humidity_enabled ? "true" : "false",
        ts.rh_valid ? "true" : "false", ts.rh_valid ? ts.rh : 0.0f,
        tc.rh_target, tc.rh_hys, humidity_sensor_name(),
        ts.reheat_active ? "true" : "false",
        (unsigned long)ts.reheat_seconds, (unsigned long)ts.rest_remaining_s,
        tc.reheat_max_dev_c, (unsigned long)tc.reheat_max_min,
        (unsigned long)tc.sensor_timeout_s, (unsigned long)tc.rh_timeout_s);

    /* pills */
    char pills[768];
    int po = snprintf(pills, sizeof(pills), "[");
    for (int i = 0; i < pill_count() && po < (int)sizeof(pills) - 176; i++) {
        pill_reading_t p;
        if (!pill_get(i, &p)) {
            continue;
        }
        int age = (int)((esp_timer_get_time() - p.last_seen_us) / 1000000);
        po += snprintf(pills + po, sizeof(pills) - po,
            "%s{\"addr\":\"%s\",\"v\":%u,\"celsius\":%.2f,\"gravity\":%.4f,"
            "\"battery\":%d,\"rssi\":%d,\"age_s\":%d,\"interval_s\":%d,"
            "\"velocity\":%.3f,\"velocity_valid\":%s}",
            (i && po > 1) ? "," : "", p.addr, p.version, p.celsius, p.gravity,
            p.battery_pct, p.rssi, age, p.interval_s, p.gravity_velocity,
            p.velocity_valid ? "true" : "false");
    }
    snprintf(pills + po, sizeof(pills) - po, "]");

    /* alarms */
    char alarms[560];
    int ao = snprintf(alarms, sizeof(alarms), "[");
    for (int i = 0; i < ALARM_COUNT && ao < (int)sizeof(alarms) - 140; i++) {
        alarm_state_t a;
        if (!alarms_get((alarm_id_t)i, &a) || !a.latched) {
            continue;
        }
        ao += snprintf(alarms + ao, sizeof(alarms) - ao,
            "%s{\"id\":\"%s\",\"text\":\"%s\",\"active\":%s,"
            "\"silenced\":%s,\"age_s\":%lu,\"count\":%lu}",
            (ao > 1) ? "," : "", alarms_key((alarm_id_t)i),
            alarms_text((alarm_id_t)i),
            a.active ? "true" : "false", a.silenced ? "true" : "false",
            (unsigned long)((esp_timer_get_time() - a.raised_us) / 1000000),
            (unsigned long)a.count);
    }
    snprintf(alarms + ao, sizeof(alarms) - ao, "]");

    char buf[3456];
    int n = snprintf(buf, sizeof(buf),
        "{"
        "\"version\":\"%s\",\"built\":\"%s %s\",\"partition\":\"%s\","
        "\"uptime_s\":%lu,\"heap\":%lu,\"rssi\":%d,\"ip\":\"%s\","
        "\"outputs\":{\"heater\":%d,\"cooler\":%d,\"fan\":%d,\"light\":%d,\"buzzer\":%d},"
        "\"inputs\":{\"up\":%d,\"down\":%d,\"select\":%d,\"back\":%d,\"door\":%d},"
        "\"runtime\":{\"total\":%lu,\"compressor\":%lu,\"heating\":%lu},"
        "\"starts\":{\"compressor\":%lu,\"heating\":%lu},"
        "\"cool_blocked_s\":%lu,\"heat_blocked_s\":%lu,"
        "\"probe\":{%s},"
        "\"thermostat\":{%s},"
        "\"pills\":%s,"
        "\"alarms\":%s,"
        "\"log_head\":%llu"
        "}",
        desc->version, desc->date, desc->time, run ? run->label : "?",
        (unsigned long)(esp_timer_get_time() / 1000000),
        (unsigned long)esp_get_free_heap_size(),
        wifi_rssi(), wifi_ip_str(),
        relays_get(RELAY_HEATER), relays_get(RELAY_COOLER),
        relays_get(RELAY_FAN), relays_get(RELAY_LIGHT), relays_get(RELAY_BUZZER),
        buttons_is_down(BTN_UP), buttons_is_down(BTN_DOWN),
        buttons_is_down(BTN_SELECT), buttons_is_down(BTN_BACK),
        buttons_is_down(BTN_DOOR),
        (unsigned long)relays_total_runtime_seconds(),
        (unsigned long)relays_runtime_seconds(RELAY_COOLER),
        (unsigned long)relays_runtime_seconds(RELAY_HEATER),
        (unsigned long)relays_start_count(RELAY_COOLER),
        (unsigned long)relays_start_count(RELAY_HEATER),
        (unsigned long)relays_cool_blocked_for(),
        (unsigned long)relays_heat_blocked_for(),
        probe, thermo, pills, alarms,
        (unsigned long long)netlog_head());

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, buf, n);
}

static esp_err_t log_get(httpd_req_t *req)
{
    uint64_t since = 0;
    char query[64];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[32];
        if (httpd_query_key_value(query, "since", val, sizeof(val)) == ESP_OK) {
            since = strtoull(val, NULL, 10);
        }
    }

    static char chunk[2048];
    uint64_t next = 0, dropped = 0;
    size_t n = netlog_read(since, chunk, sizeof(chunk), &next, &dropped);

    char hdr[24];
    snprintf(hdr, sizeof(hdr), "%llu", (unsigned long long)next);
    httpd_resp_set_hdr(req, "X-Log-Next", hdr);

    static char dhdr[24];
    snprintf(dhdr, sizeof(dhdr), "%llu", (unsigned long long)dropped);
    httpd_resp_set_hdr(req, "X-Log-Dropped", dhdr);

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, chunk, n);
}

static esp_err_t relay_post(httpd_req_t *req)
{
    char query[96], name[16], onv[8];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK ||
        httpd_query_key_value(query, "on", onv, sizeof(onv)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "need name and on");
        return ESP_FAIL;
    }
    bool on = (onv[0] == '1');

    esp_err_t err;
    if (!strcmp(name, "heater")) {
        err = relays_request_heat(on);
    } else if (!strcmp(name, "cooler")) {
        err = relays_request_cool(on);
    } else if (!strcmp(name, "fan")) {
        err = relays_set(RELAY_FAN, on);
    } else if (!strcmp(name, "light")) {
        err = relays_set(RELAY_LIGHT, on);
    } else if (!strcmp(name, "buzzer")) {
        err = relays_set(RELAY_BUZZER, on);
    } else if (!strcmp(name, "allof")) {
        relays_emergency_stop();
        relays_set(RELAY_FAN, false);
        relays_set(RELAY_LIGHT, false);
        relays_set(RELAY_BUZZER, false);
        err = ESP_OK;
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown output");
        return ESP_FAIL;
    }

    if (err == ESP_ERR_INVALID_STATE) {
        /* Not an error - protection refused it. Say so plainly. */
        uint32_t wait = !strcmp(name, "cooler") ? relays_cool_blocked_for()
                                                : relays_heat_blocked_for();
        char msg[96];
        int n = snprintf(msg, sizeof(msg),
                         "{\"applied\":false,\"blocked_s\":%lu}",
                         (unsigned long)wait);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, msg, n);
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, esp_err_to_name(err));
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"applied\":true}", HTTPD_RESP_USE_STRLEN);
}

/* ------------------------------------------------------------------ */

static bool q_float(const char *q, const char *key, float *out)
{
    char v[24];
    if (httpd_query_key_value(q, key, v, sizeof(v)) != ESP_OK) {
        return false;
    }
    char *end = NULL;
    float f = strtof(v, &end);
    if (end == v) {
        return false;
    }
    *out = f;
    return true;
}

static bool q_bool(const char *q, const char *key, bool *out)
{
    char v[8];
    if (httpd_query_key_value(q, key, v, sizeof(v)) != ESP_OK) {
        return false;
    }
    *out = (v[0] == '1' || v[0] == 't' || v[0] == 'o' || v[0] == 'y');
    return true;
}

static esp_err_t thermostat_post(httpd_req_t *req)
{
    char query[256];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no parameters");
        return ESP_FAIL;
    }

    thermostat_config_t c;
    thermostat_get_config(&c);

    q_bool(query, "enabled", &c.enabled);
    q_float(query, "target", &c.target_c);
    q_float(query, "cooler_hys", &c.cooler_hys);
    q_float(query, "heater_hys", &c.heater_hys);

    char v[32];
    if (httpd_query_key_value(query, "source", v, sizeof(v)) == ESP_OK) {
        c.source = !strcmp(v, "pill") ? TEMP_SOURCE_PILL : TEMP_SOURCE_INTERNAL;
    }
    if (httpd_query_key_value(query, "fan", v, sizeof(v)) == ESP_OK) {
        if (!strcmp(v, "always_on")) {
            c.fan_mode = FAN_MODE_ALWAYS_ON;
        } else if (!strcmp(v, "off") || !strcmp(v, "always_off")) {
            c.fan_mode = FAN_MODE_ALWAYS_OFF;
        } else {
            c.fan_mode = FAN_MODE_AUTOMATIC;
        }
    }
    if (httpd_query_key_value(query, "pill", v, sizeof(v)) == ESP_OK) {
        strlcpy(c.pill_addr, v, sizeof(c.pill_addr));
    }

    /* humidity / reheat dehumidification */
    q_bool(query, "humidity", &c.humidity_enabled);
    q_float(query, "rh_target", &c.rh_target);
    q_float(query, "rh_hys", &c.rh_hys);
    q_float(query, "reheat_max_dev", &c.reheat_max_dev_c);
    float f;
    if (q_float(query, "reheat_max_min", &f) && f >= 1.0f) {
        c.reheat_max_min = (uint32_t)f;
    }
    if (q_float(query, "reheat_rest_min", &f) && f >= 0.0f) {
        c.reheat_rest_min = (uint32_t)f;
    }
    if (q_float(query, "sensor_timeout", &f) && f >= 30.0f) {
        c.sensor_timeout_s = (uint32_t)f;
    }
    if (q_float(query, "rh_timeout", &f) && f >= 15.0f) {
        c.rh_timeout_s = (uint32_t)f;
    }

    if (c.humidity_enabled && humidity_sensor_type() == HUM_SENSOR_NONE) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "no humidity sensor detected on the spare header");
        return ESP_FAIL;
    }
    if (c.rh_target < 20.0f || c.rh_target > 95.0f || c.rh_hys <= 0.0f) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "rh_target must be 20..95 and rh_hys positive");
        return ESP_FAIL;
    }
    if (c.reheat_max_dev_c <= 0.0f || c.reheat_max_dev_c > 5.0f) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "reheat_max_dev must be 0..5 degrees");
        return ESP_FAIL;
    }

    esp_err_t err = thermostat_set_config(&c);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "rejected: check target is within min/max and "
                            "hysteresis is positive");
        return ESP_FAIL;
    }

    bool save = false;
    if (q_bool(query, "save", &save) && save) {
        thermostat_save();
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

/*
 * Probe calibration.
 *
 *   POST /api/calibrate?point=0&actual=23.2     capture against a known value
 *   POST /api/calibrate?point=0&from=pill       capture against the Pill
 *   POST /api/calibrate?offset=-4.8             constant trim
 *   POST /api/calibrate?clear=1                 forget everything
 *
 * Add &save=1 to persist. The Pill is factory-calibrated, so `from=pill` with
 * both sensors sitting in the same place is the easiest way to correct a
 * never-commissioned board.
 */
static esp_err_t calibrate_post(httpd_req_t *req)
{
    char query[160];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no parameters");
        return ESP_FAIL;
    }

    bool clear = false;
    if (q_bool(query, "clear", &clear) && clear) {
        temp_probe_clear_cal();
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":true,\"cleared\":true}",
                               HTTPD_RESP_USE_STRLEN);
    }

    float off;
    if (q_float(query, "offset", &off)) {
        temp_probe_set_offset(off);
    } else {
        float pt;
        if (!q_float(query, "point", &pt) || (pt != 0.0f && pt != 1.0f)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                "need point=0|1 with actual=, or offset=, or clear=1");
            return ESP_FAIL;
        }

        float actual;
        char src[16];
        if (httpd_query_key_value(query, "from", src, sizeof(src)) == ESP_OK &&
            !strcmp(src, "pill")) {
            pill_reading_t pr;
            if (!pill_get_by_addr(NULL, &pr)) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                    "no Pill reading available to calibrate against");
                return ESP_FAIL;
            }
            int age = (int)((esp_timer_get_time() - pr.last_seen_us) / 1000000);
            if (age > 900) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                    "Pill reading is too old to calibrate against");
                return ESP_FAIL;
            }
            actual = pr.celsius;
        } else if (!q_float(query, "actual", &actual)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                "need actual= or from=pill");
            return ESP_FAIL;
        }

        esp_err_t err = temp_probe_capture((int)pt, actual);
        if (err != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                "probe not readable; cannot calibrate");
            return ESP_FAIL;
        }
    }

    bool save = false;
    if (q_bool(query, "save", &save) && save) {
        temp_probe_save_cal();
    }

    probe_cal_t c;
    temp_probe_get_cal(&c);
    char msg[192];
    int n = snprintf(msg, sizeof(msg),
        "{\"ok\":true,\"mode\":\"%s\",\"m0\":%.2f,\"a0\":%.2f,"
        "\"m1\":%.2f,\"a1\":%.2f,\"offset\":%.2f}",
        c.two_point ? "two_point" : (c.have0 ? "one_point" : "offset"),
        c.measured0, c.actual0, c.measured1, c.actual1, c.offset);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, msg, n);
}

/*
 * POST /api/alarm?action=silence|dismiss[&id=<key>]
 *
 * Silence and dismiss are deliberately separate: silence stops the buzzer and
 * leaves the alarm showing, dismiss clears it. Dismissing an alarm whose
 * condition is still true is refused rather than faked - it just silences.
 */
static esp_err_t alarm_post(httpd_req_t *req)
{
    char query[96], action[16], idkey[24];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "action", action, sizeof(action)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "need action=silence|dismiss");
        return ESP_FAIL;
    }
    bool silence = !strcmp(action, "silence");
    bool dismiss = !strcmp(action, "dismiss");
    if (!silence && !dismiss) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown action");
        return ESP_FAIL;
    }

    if (httpd_query_key_value(query, "id", idkey, sizeof(idkey)) == ESP_OK) {
        int found = -1;
        for (int i = 0; i < ALARM_COUNT; i++) {
            if (!strcmp(alarms_key((alarm_id_t)i), idkey)) {
                found = i;
                break;
            }
        }
        if (found < 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown alarm id");
            return ESP_FAIL;
        }
        if (silence) {
            alarms_silence((alarm_id_t)found);
        } else {
            alarms_dismiss((alarm_id_t)found);
        }
    } else {
        if (silence) {
            alarms_silence_all();
        } else {
            alarms_dismiss_all();
        }
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

/* ------------------------------------------------------------------ */

static void restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(700));   /* let the HTTP response drain */
    esp_restart();
}

/*
 * POST /api/orient?x=0|1&y=0|1 - flip the panel mirror without a rebuild.
 * A bench aid for settling which way round the glass is mounted; the answer
 * belongs in UI_DEFAULT_MIRROR_X/Y once it is known.
 */
static esp_err_t orient_post(httpd_req_t *req)
{
    char q[64];
    bool mx, my;
    ui_get_mirror(&mx, &my);

    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        char v[8];
        if (httpd_query_key_value(q, "x", v, sizeof(v)) == ESP_OK) mx = (v[0] == '1');
        if (httpd_query_key_value(q, "y", v, sizeof(v)) == ESP_OK) my = (v[0] == '1');
    }
    ui_set_mirror(mx, my);

    char body[64];
    int n = snprintf(body, sizeof(body), "{\"x\":%d,\"y\":%d}", mx, my);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, n);
}

static esp_err_t ota_post(httpd_req_t *req)
{
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "no OTA partition");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA -> %s (%d bytes incoming)",
             target->label, req->content_len);

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            esp_err_to_name(err));
        return ESP_FAIL;
    }

    static char buf[2048];
    int remaining = req->content_len;
    int written = 0;

    while (remaining > 0) {
        int want = remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf);
        int got = httpd_req_recv(req, buf, want);
        if (got == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (got <= 0) {
            ESP_LOGE(TAG, "upload aborted after %d bytes", written);
            esp_ota_abort(handle);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "upload aborted");
            return ESP_FAIL;
        }
        err = esp_ota_write(handle, buf, got);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err));
            esp_ota_abort(handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                esp_err_to_name(err));
            return ESP_FAIL;
        }
        written   += got;
        remaining -= got;
    }

    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        /* ESP_ERR_OTA_VALIDATE_FAILED here means the image is not valid. */
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, esp_err_to_name(err));
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_boot_partition: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            esp_err_to_name(err));
        return ESP_FAIL;
    }

    ESP_LOGW(TAG, "OTA complete (%d bytes) -> %s, restarting",
             written, target->label);

    char msg[128];
    int n = snprintf(msg, sizeof(msg),
                     "{\"ok\":true,\"bytes\":%d,\"partition\":\"%s\"}",
                     written, target->label);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, msg, n);

    xTaskCreate(restart_task, "ota_restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */

void webui_mark_app_valid(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t state;

    if (esp_ota_get_state_partition(run, &state) != ESP_OK) {
        return;
    }
    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
            ESP_LOGI(TAG, "image on %s marked valid; rollback cancelled",
                     run->label);
        } else {
            ESP_LOGE(TAG, "failed to mark image valid - will roll back");
        }
    }
}

esp_err_t webui_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size        = 8192;     /* OTA path needs headroom */
    cfg.max_uri_handlers  = 12;
    cfg.lru_purge_enable  = true;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t routes[] = {
        { .uri = "/",           .method = HTTP_GET,  .handler = root_get },
        { .uri = "/api/status", .method = HTTP_GET,  .handler = status_get },
        { .uri = "/api/log",    .method = HTTP_GET,  .handler = log_get },
        { .uri = "/api/relay",  .method = HTTP_POST, .handler = relay_post },
        { .uri = "/api/thermostat", .method = HTTP_POST, .handler = thermostat_post },
        { .uri = "/api/calibrate",  .method = HTTP_POST, .handler = calibrate_post },
        { .uri = "/api/alarm",      .method = HTTP_POST, .handler = alarm_post },
        { .uri = "/api/orient", .method = HTTP_POST, .handler = orient_post },
        { .uri = "/api/ota",    .method = HTTP_POST, .handler = ota_post },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(s_server, &routes[i]);
    }

    ESP_LOGI(TAG, "web ui at http://%s/", wifi_ip_str());
    return ESP_OK;
}

esp_err_t webui_stop(void)
{
    if (!s_server) {
        return ESP_OK;
    }
    esp_err_t err = httpd_stop(s_server);
    s_server = NULL;
    return err;
}
