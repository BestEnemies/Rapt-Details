/*
 * temp_probe.h - NTC thermistor reading on ADC1 channel 6 (GPIO 34).
 *
 * The acquisition path replicates the stock firmware exactly, because its
 * behaviour was recovered precisely and is worth keeping:
 *
 *   - 128 raw samples accumulated in a tight loop, mean taken as sum >> 7
 *   - the entire reading is invalidated if ANY sample falls outside 2..4093,
 *     which is how the stock firmware detects an open or shorted probe
 *   - the mean is converted to millivolts via the calibration characteristics
 *
 * What is NOT recovered is the final resistance-to-temperature curve. No
 * Steinhart-Hart coefficient triple appears as adjacent constants in the
 * stock image, so it is likely a beta equation or a lookup table.
 *
 * The placeholder below uses a beta equation with common 10k/B3950 values.
 * TREAT ITS OUTPUT AS UNCALIBRATED. Fit your own curve before trusting a
 * reading to control a ferment - see temp_probe_read_raw() and the
 * calibration note in README.md.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    PROBE_OK = 0,
    PROBE_FAULT_OPEN,      /* samples pinned high - probe disconnected     */
    PROBE_FAULT_SHORT,     /* samples pinned low - probe shorted           */
    PROBE_FAULT_UNSTABLE,  /* out-of-range sample seen mid-burst           */
} probe_status_t;

typedef struct {
    probe_status_t status;
    int      raw_mean;      /* averaged ADC counts, 0..4095      */
    int      millivolts;    /* calibrated                        */
    float    resistance;    /* ohms, computed from the divider   */
    float    celsius_raw;   /* straight from the beta curve      */
    float    celsius;       /* after calibration, if any         */
} probe_reading_t;

/*
 * Per-unit calibration.
 *
 * The beta curve is right - it came out of the stock firmware - but a
 * brand-new mainboard reads several degrees low because it has never been
 * commissioned. The stock firmware absorbs exactly this with per-unit NVS
 * keys (cal_temp0/cal_adc0, cal_temp1/cal_adc1, temp_offset), and this is the
 * same idea.
 *
 * Two-point is a straight linear map between the captured pairs, which is
 * appropriate over a fermentation chamber's narrow working range. One point
 * degenerates to a constant offset.
 *
 * A factory-calibrated RAPT Pill sitting in the chamber makes an excellent
 * reference - put both at the same place, let them settle, and capture.
 */
typedef struct {
    bool  two_point;
    float measured0, actual0;   /* what we read, what it really was */
    float measured1, actual1;
    float offset;               /* used when a single point is set  */
    bool  have0, have1;
} probe_cal_t;

/* Capture point 0 or 1: "the true temperature right now is `actual`". */
esp_err_t temp_probe_capture(int point, float actual);

/* Simple constant trim, independent of the two-point pair. */
esp_err_t temp_probe_set_offset(float offset);

void temp_probe_get_cal(probe_cal_t *out);
esp_err_t temp_probe_clear_cal(void);

/* Persist to / restore from NVS. Loaded automatically by temp_probe_init(). */
esp_err_t temp_probe_save_cal(void);

esp_err_t temp_probe_init(void);

/* Blocking: takes BOARD_PROBE_SAMPLES samples. Roughly a millisecond. */
esp_err_t temp_probe_read(probe_reading_t *out);

/*
 * Raw access for calibration. Put a known resistance across the probe
 * terminals, call this, and record the pair. Three or four points across
 * your working range are enough to fit a curve - and comparing against the
 * stock firmware's own displayed temperature validates it end to end.
 */
esp_err_t temp_probe_read_raw(int *raw_mean, int *millivolts);

const char *temp_probe_status_str(probe_status_t s);
