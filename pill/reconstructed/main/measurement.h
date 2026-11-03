// measurement.h — sensor acquisition + telemetry assembly (reverse-engineered)
//
// Reconstructed from app_2.0.0.bin (source file: measurement.cpp). Confidence:
// High for the TMP1075 driver, task/queue model and NVS key set; Medium for the
// report/automation payload builder (represented structurally).
//
// The module owns a dedicated FreeRTOS "measurement" task and a pair of queues:
//   measurement_start    (5 x 4 bytes)  — request a reading
//   measurement_finished (5 x 40 bytes) — completed reading record
// A reading couples the accelerometer tilt (see accel) with the TMP1075
// temperature; gravity is then derived from tilt via gravity_calc.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// A completed reading (the 40-byte measurement_finished record; exact field
// order partly inferred). Temperatures in °C, tilt angle in degrees.
typedef struct {
    int      status;          // 0 = ok, else esp_err_t (e.g. ESP_ERR_TIMEOUT)
    float    temperature;     // TMP1075 reading, offset-corrected
    float    angle;           // accelerometer tilt angle
    float    gravity;         // derived specific gravity
    float    accel_x, accel_y, accel_z; /* inferred */
    uint32_t calibration_state;
} measurement_t;

void  measurement_init(void);                 // FUN_0x400e2254  (measurement.cpp:0x140)
bool  measurement_read_temperature(float* out);// TMP1075 one-shot read (FUN_0x400e7eb4)

// Register all persistent settings keys (defaults) with the nonvolatile layer.
void  settings_register_keys(void);           // FUN_0x400e25a8

#ifdef __cplusplus
}
#endif
