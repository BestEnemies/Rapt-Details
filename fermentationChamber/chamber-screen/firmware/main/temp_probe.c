#include "temp_probe.h"
#include "board.h"

#include <math.h>
#include <string.h>
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "temp_probe";

#define CAL_NAMESPACE  "chamber"
#define CAL_VERSION    1

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t         s_cali;
static bool                      s_cali_ok;
static probe_cal_t               s_cal;

static void load_cal(void);

/*
 * Map a raw curve temperature onto a corrected one.
 *
 * Two-point is a straight line through the captured pairs. Note this
 * extrapolates outside them - which is intended, since a chamber will
 * sometimes sit beyond its calibration points, and a linear extension of a
 * measured slope beats no correction at all over a few degrees.
 */
static float apply_calibration(float raw)
{
    if (isnan(raw)) {
        return raw;
    }
    if (s_cal.two_point) {
        float dm = s_cal.measured1 - s_cal.measured0;
        if (fabsf(dm) > 0.001f) {
            float slope = (s_cal.actual1 - s_cal.actual0) / dm;
            return s_cal.actual0 + (raw - s_cal.measured0) * slope;
        }
    }
    if (s_cal.have0) {
        return raw + (s_cal.actual0 - s_cal.measured0);
    }
    return raw + s_cal.offset;
}

/* -------------------------------------------------------------------------
 * Thermistor curve, recovered from the stock firmware rather than guessed.
 *
 *   1/T = 1/ntc_t0 + (1/ntc_beta0) * ln(R / ntc_r0)
 *
 * with ntc_t0 already in Kelvin. Constants are the stock defaults from the
 * table at 0x3f40e0c4; see board.h.
 *
 * NOT YET IMPLEMENTED, and present in the stock firmware:
 *   - two-point ADC correction (cal_temp0/cal_adc0, cal_temp1/cal_adc1)
 *   - temp_offset
 *   - the second probe channel's ntc_beta1/ntc_t1/ntc_r1
 * A production unit may carry per-unit values for these in NVS, so expect a
 * residual error against the stock reading until they are handled.
 * ---------------------------------------------------------------------- */
static float resistance_to_celsius(float r_ohms)
{
    if (r_ohms <= 0.0f) {
        return NAN;
    }
    float inv_t = (1.0f / BOARD_NTC_T0_KELVIN)
                + (1.0f / BOARD_NTC_BETA) * logf(r_ohms / BOARD_NTC_R0_OHMS);
    return (1.0f / inv_t) - 273.15f;
}

/*
 * Divider maths - matches the stock routine instruction for instruction:
 *
 *     R_ntc = R_series * V / (Vsupply - V)
 *
 * Thermistor on the low side, 56 kohm series resistor, 3300 mV supply.
 */
static float millivolts_to_resistance(int mv)
{
    float v = (float)mv;
    float denom = BOARD_PROBE_SUPPLY_MV - v;
    if (denom <= 1.0f) {
        return INFINITY;
    }
    return BOARD_PROBE_SERIES_OHMS * v / denom;
}

esp_err_t temp_probe_init(void)
{
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = BOARD_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_adc);
    if (err != ESP_OK) {
        return err;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = BOARD_ADC_ATTEN,
        .bitwidth = BOARD_ADC_BITWIDTH,
    };
    err = adc_oneshot_config_channel(s_adc, BOARD_ADC_CHANNEL, &chan_cfg);
    if (err != ESP_OK) {
        return err;
    }

    /*
     * The ESP32 supports line-fitting calibration only. The stock firmware
     * passes a 1100 mV default Vref and reports whether two-point or Vref
     * calibration is burned into efuse; the driver picks the best available.
     */
    adc_cali_line_fitting_config_t cali_cfg = {
        .unit_id      = BOARD_ADC_UNIT,
        .atten        = BOARD_ADC_ATTEN,
        .bitwidth     = BOARD_ADC_BITWIDTH,
        .default_vref = 1100,
    };
    s_cali_ok = (adc_cali_create_scheme_line_fitting(&cali_cfg, &s_cali) == ESP_OK);
    if (!s_cali_ok) {
        ESP_LOGW(TAG, "no ADC calibration available; voltages are approximate");
    }

    load_cal();

    ESP_LOGI(TAG, "init: ADC1 ch%d (GPIO34), %d samples/reading",
             (int)BOARD_ADC_CHANNEL, BOARD_PROBE_SAMPLES);
    return ESP_OK;
}

/* Returns the accumulated sum and flags any out-of-range sample. */
static esp_err_t sample_burst(int *sum_out, bool *in_range,
                              int *min_out, int *max_out)
{
    int sum = 0, mn = 4095, mx = 0;
    bool ok = true;

    for (int i = 0; i < BOARD_PROBE_SAMPLES; i++) {
        int raw = 0;
        esp_err_t err = adc_oneshot_read(s_adc, BOARD_ADC_CHANNEL, &raw);
        if (err != ESP_OK) {
            return err;
        }
        if (raw < BOARD_PROBE_RAW_MIN || raw > BOARD_PROBE_RAW_MAX) {
            ok = false;
        }
        if (raw < mn) mn = raw;
        if (raw > mx) mx = raw;
        sum += raw;
    }

    *sum_out  = sum;
    *in_range = ok;
    *min_out  = mn;
    *max_out  = mx;
    return ESP_OK;
}

esp_err_t temp_probe_read(probe_reading_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }

    int sum, mn, mx;
    bool in_range;
    esp_err_t err = sample_burst(&sum, &in_range, &mn, &mx);
    if (err != ESP_OK) {
        return err;
    }

    /* Stock firmware uses sum >> 7, i.e. an exact divide by 128. */
    int mean = sum / BOARD_PROBE_SAMPLES;

    out->raw_mean = mean;

    if (!in_range) {
        /* Distinguish the fault so the UI can say something useful. */
        if (mx > BOARD_PROBE_RAW_MAX && mn > BOARD_PROBE_RAW_MAX) {
            out->status = PROBE_FAULT_OPEN;
        } else if (mn < BOARD_PROBE_RAW_MIN && mx < BOARD_PROBE_RAW_MIN) {
            out->status = PROBE_FAULT_SHORT;
        } else {
            out->status = PROBE_FAULT_UNSTABLE;
        }
        out->millivolts  = 0;
        out->resistance  = NAN;
        out->celsius_raw = NAN;
        out->celsius     = NAN;
        return ESP_OK;
    }

    int mv = mean;
    if (s_cali_ok) {
        adc_cali_raw_to_voltage(s_cali, mean, &mv);
    }

    out->status      = PROBE_OK;
    out->millivolts  = mv;
    out->resistance  = millivolts_to_resistance(mv);
    out->celsius_raw = resistance_to_celsius(out->resistance);
    out->celsius     = apply_calibration(out->celsius_raw);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* calibration                                                          */

esp_err_t temp_probe_capture(int point, float actual)
{
    if (point != 0 && point != 1) {
        return ESP_ERR_INVALID_ARG;
    }
    probe_reading_t r;
    esp_err_t err = temp_probe_read(&r);
    if (err != ESP_OK) {
        return err;
    }
    if (r.status != PROBE_OK || isnan(r.celsius_raw)) {
        ESP_LOGW(TAG, "cannot calibrate: probe is %s",
                 temp_probe_status_str(r.status));
        return ESP_ERR_INVALID_STATE;
    }

    if (point == 0) {
        s_cal.measured0 = r.celsius_raw;
        s_cal.actual0   = actual;
        s_cal.have0     = true;
    } else {
        s_cal.measured1 = r.celsius_raw;
        s_cal.actual1   = actual;
        s_cal.have1     = true;
    }

    /*
     * Two points only make sense if they are meaningfully far apart - fitting
     * a line through two nearly-identical temperatures amplifies noise into a
     * wildly wrong slope. Below that separation, fall back to an offset.
     */
    s_cal.two_point = s_cal.have0 && s_cal.have1 &&
                      fabsf(s_cal.measured1 - s_cal.measured0) >= 5.0f;

    if (s_cal.have0 && s_cal.have1 && !s_cal.two_point) {
        ESP_LOGW(TAG, "points are only %.1fC apart; using offset, not a slope",
                 fabsf(s_cal.measured1 - s_cal.measured0));
    }

    ESP_LOGI(TAG, "cal point %d: measured %.2f, actual %.2f (%s)",
             point, r.celsius_raw, actual,
             s_cal.two_point ? "two-point" : "offset");
    return ESP_OK;
}

esp_err_t temp_probe_set_offset(float offset)
{
    s_cal.offset = offset;
    ESP_LOGI(TAG, "cal offset set to %+.2fC", offset);
    return ESP_OK;
}

void temp_probe_get_cal(probe_cal_t *out)
{
    if (out) {
        *out = s_cal;
    }
}

esp_err_t temp_probe_clear_cal(void)
{
    memset(&s_cal, 0, sizeof(s_cal));
    ESP_LOGI(TAG, "calibration cleared");
    return temp_probe_save_cal();
}

esp_err_t temp_probe_save_cal(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(CAL_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, "probecal", &s_cal, sizeof(s_cal));
    if (err == ESP_OK) {
        err = nvs_set_u32(h, "probecal_v", CAL_VERSION);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static void load_cal(void)
{
    nvs_handle_t h;
    if (nvs_open(CAL_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint32_t ver = 0;
    if (nvs_get_u32(h, "probecal_v", &ver) != ESP_OK || ver != CAL_VERSION) {
        nvs_close(h);
        return;
    }
    probe_cal_t tmp;
    size_t len = sizeof(tmp);
    if (nvs_get_blob(h, "probecal", &tmp, &len) == ESP_OK && len == sizeof(tmp)) {
        s_cal = tmp;
        ESP_LOGI(TAG, "loaded calibration (%s)",
                 s_cal.two_point ? "two-point" :
                 (s_cal.have0 || s_cal.offset != 0.0f) ? "offset" : "none");
    }
    nvs_close(h);
}

esp_err_t temp_probe_read_raw(int *raw_mean, int *millivolts)
{
    int sum, mn, mx;
    bool in_range;
    esp_err_t err = sample_burst(&sum, &in_range, &mn, &mx);
    if (err != ESP_OK) {
        return err;
    }
    int mean = sum / BOARD_PROBE_SAMPLES;
    if (raw_mean) {
        *raw_mean = mean;
    }
    if (millivolts) {
        int mv = mean;
        if (s_cali_ok) {
            adc_cali_raw_to_voltage(s_cali, mean, &mv);
        }
        *millivolts = mv;
    }
    return ESP_OK;
}

const char *temp_probe_status_str(probe_status_t s)
{
    switch (s) {
    case PROBE_OK:             return "ok";
    case PROBE_FAULT_OPEN:     return "open circuit";
    case PROBE_FAULT_SHORT:    return "short circuit";
    case PROBE_FAULT_UNSTABLE: return "unstable";
    default:                   return "?";
    }
}
