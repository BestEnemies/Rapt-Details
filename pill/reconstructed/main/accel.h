// =============================================================================
// accel.h — RAPT Pill 2.0.0 accelerometer / gesture module (RECONSTRUCTED)
// Reverse-engineered from app_2.0.0.bin. Source files: accel.c, lis3dh.c,
// gesture.*  Not original source. Confidence: HIGH (chip+regs+flow corroborated
// by embedded strings "LIS3DH detected/configured", WHO_AM_I check).
//
// Device: ST LIS3DH 3-axis accelerometer on I²C (7-bit addr 0x19, SDO=1;
// write addr byte 0x32, read addr byte 0x33). Used for (a) tilt angle → gravity
// and (b) tap/"inverted hold" gesture detection to wake the Pill into a
// connectable session.
// =============================================================================
#pragma once
#include <stdint.h>
#include <stdbool.h>

// ---- LIS3DH register map (subset actually used) -----------------------------
enum lis3dh_reg {
    LIS3DH_WHO_AM_I    = 0x0F,  // expect 0x33
    LIS3DH_CTRL_REG0   = 0x1E,
    LIS3DH_TEMP_CFG    = 0x1F,
    LIS3DH_CTRL_REG1   = 0x20,  // ODR + axis enable
    LIS3DH_CTRL_REG2   = 0x21,  // HPF
    LIS3DH_CTRL_REG3   = 0x22,  // INT1 routing
    LIS3DH_CTRL_REG4   = 0x23,  // BDU/scale/HR
    LIS3DH_CTRL_REG5   = 0x24,  // FIFO enable / latch
    LIS3DH_CTRL_REG6   = 0x25,
    LIS3DH_REFERENCE   = 0x26,
    LIS3DH_STATUS      = 0x27,
    LIS3DH_OUT_X_L     = 0x28,  // auto-increment read base (|0x80)
    LIS3DH_FIFO_CTRL   = 0x2E,
    LIS3DH_FIFO_SRC    = 0x2F,
    LIS3DH_INT1_CFG    = 0x30,
    LIS3DH_INT1_SRC    = 0x31,
    LIS3DH_INT1_THS    = 0x32,
    LIS3DH_INT1_DUR    = 0x33,
    LIS3DH_CLICK_CFG   = 0x38,
    LIS3DH_CLICK_SRC   = 0x39,
    LIS3DH_CLICK_THS   = 0x3A,
    LIS3DH_TIME_LIMIT  = 0x3B,
    LIS3DH_TIME_LATENCY= 0x3C,
    LIS3DH_TIME_WINDOW = 0x3D,
};

// ---- gesture engine state (global, @0x400d12a0) -----------------------------
enum gesture_state {
    GESTURE_IDLE         = 0,  // waiting for the first strong tilt/tap
    GESTURE_ARMED        = 1,  // listening for a double-tap jerk pattern
    GESTURE_SAMPLING     = 2,  // draining the FIFO, scoring jerk/clicks
    GESTURE_INVERT_HOLD  = 3,  // detecting a sustained inverted hold
    GESTURE_SESSION_WAIT = 4,  // a session was opened; hold until it ends
};

int  accel_init_peripherals(void);  // FUN_0x400e046c — I²C bring-up + LIS3DH detect
void accel_configure(void);         // FUN_0x400e05a4 — "Configuring LIS3DH"
int  accel_power_down(void);        // FUN_0x400e0694
void accel_click_engine(bool arm, uint8_t cfg, uint32_t time_limit,
                        uint8_t latency, uint8_t window);  // FUN_0x400e09e4
void gesture_tick(void);            // FUN_0x400df26c — called from the measurement loop
void gesture_log_serialise(void *out_stream);  // FUN_0x400dedec — JSON for `export gesture_log`
int  diag_export(void);             // FUN_0x400dea2c — paged diagnostics export
void status_led_tick(void);         // FUN_0x400e6bc8 — LEDC status LED
