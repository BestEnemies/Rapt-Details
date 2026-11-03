/*
 * humidity.h - Sensirion SHT3x / SHT4x humidity and temperature sensor on the
 * spare I2C header (SDA = GPIO 25, SCL = GPIO 26).
 *
 * Both parts live at 0x44 (SHT3x can be strapped to 0x45) and return the same
 * 6-byte frame - two big-endian words each followed by a CRC8 - so one driver
 * covers both. They are told apart at init by whether they answer the SHT4x
 * serial-number command.
 *
 * Every reading is CRC-checked. In a fridge the sensor lines are long and the
 * environment is wet, so a corrupted word that happens to look plausible is a
 * real failure mode - and a bogus humidity reading would drive the compressor.
 *
 * Condensation note: when the chamber passes dew point the sensor gets wet and
 * reads ~100% until it dries. That is the sensor telling the truth about its
 * own surface, not a fault, but it means a filtered part (SHT85, or SHT31 with
 * the PTFE cap) and sensible mounting away from the evaporator matter.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    HUM_SENSOR_NONE = 0,
    HUM_SENSOR_SHT3X,
    HUM_SENSOR_SHT4X,
} humidity_sensor_t;

typedef struct {
    bool  valid;
    float rh;            /* percent, 0..100 */
    float celsius;       /* the sensor's own temperature */
    int64_t last_us;
} humidity_reading_t;

/* Returns ESP_ERR_NOT_FOUND if no sensor answers - which is not fatal. */
esp_err_t humidity_init(void);

humidity_sensor_t humidity_sensor_type(void);
const char *humidity_sensor_name(void);

/* Latest reading, refreshed by an internal task. */
bool humidity_get(humidity_reading_t *out);

/* Seconds since the last good reading, or -1 if there has never been one. */
int humidity_age_seconds(void);
