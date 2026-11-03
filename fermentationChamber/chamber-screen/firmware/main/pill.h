/*
 * pill.h - RAPT Pill / RAPT Temp BLE reception.
 *
 * The Pill is a broadcaster: it advertises its readings and never needs to be
 * connected to. We run NimBLE as a passive observer, which costs almost
 * nothing and means the sensor's battery life is unaffected by us listening.
 *
 * Frame format per KegLand's published specification and the reference
 * parser at https://github.com/sairon/rapt-ble, verified here against that
 * project's test vectors:
 *
 *   offset  size  field
 *   ------  ----  ------------------------------------------------------
 *     0      2    company id: "RA" (0x4152 LE) or "KE" (0x454b LE)
 *     2      2    magic "PT"
 *     4      1    version (1 or 2)
 *     5      6    v1: sensor MAC
 *                 v2: [0] reserved, [1] velocity valid, [2..5] float32 BE
 *    11      2    temperature, uint16 BE   -> /128 - 273.15 = degC
 *    13      4    gravity,     float32 BE  -> /1000 = specific gravity
 *    17      2    accel X,     int16 BE    -> /16
 *    19      2    accel Y,     int16 BE    -> /16
 *    21      2    accel Z,     int16 BE    -> /16
 *    23      2    battery,     int16 BE    -> /256 = percent
 *
 * Total 25 bytes of manufacturer-specific data. Note everything after the
 * company id is BIG endian, which is unusual for BLE and easy to get wrong.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define PILL_MAX_SENSORS   4
#define PILL_ADDR_STR_LEN  18      /* "aa:bb:cc:dd:ee:ff" + NUL */

typedef struct {
    bool     valid;
    char     addr[PILL_ADDR_STR_LEN];
    uint8_t  version;
    float    celsius;
    float    gravity;          /* specific gravity, e.g. 1.0109 */
    float    gravity_velocity; /* points/day; only meaningful if velocity_valid */
    bool     velocity_valid;
    int      battery_pct;
    int      rssi;
    float    accel_x, accel_y, accel_z;
    int64_t  last_seen_us;
    /*
     * Observed broadcast period in seconds, 0 until two wakes have been seen.
     *
     * A Pill wakes, advertises several times in a burst, then sleeps - often
     * for 5 minutes, configurable in the RAPT portal. Gaps shorter than
     * PILL_BURST_GAP_S are treated as the same wake and ignored, so this
     * measures the sleep period rather than the intra-burst spacing.
     *
     * Worth having because it lets the staleness timeout be checked against
     * what the sensor actually does instead of what we assumed it does.
     */
    int      interval_s;
} pill_reading_t;

/* Adverts closer together than this belong to the same wake. */
#define PILL_BURST_GAP_S   10

esp_err_t pill_start(void);

/* Number of distinct sensors heard since boot. */
int pill_count(void);

/* Copy sensor `idx` (0..pill_count-1). False if idx is out of range. */
bool pill_get(int idx, pill_reading_t *out);

/*
 * Copy the reading for a specific address, or - if addr is NULL or empty -
 * the most recently heard sensor. False if nothing matches.
 */
bool pill_get_by_addr(const char *addr, pill_reading_t *out);

/* Seconds since a sensor was last heard, or -1 if never. */
int pill_age_seconds(const char *addr);
