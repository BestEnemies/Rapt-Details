// registration.h — RAPT Pill 2.0.0, reverse-engineered (readable reconstruction)
//
// Device registration ("claim"), the connection-request refcount that brings
// the soft-AP / wifi-manager up and down, and the top-level `networking` task.
//
// Provenance: app_2.0.0.bin, log tags "autoreg" and "conn_req".
// See docs/RECONSTRUCTION_NOTES.md.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Soft-AP (factory / config access point) credentials, baked into the image.
#define FACTORY_AP_SSID      "RAPT Pill"   // @0x400d20b4
#define FACTORY_AP_PASSWORD  "kegland1"    // @0x400d2180
#define FACTORY_AP_GATEWAY   "172.217.28.1"// captive-portal gateway IP

// Automatic-registration state machine (NVS/RAM state DAT_400d09d4).
enum autoreg_state {
    AUTOREG_IDLE                 = 0,
    AUTOREG_WAIT_FIRST_CONNECTION= 1, // credentials stored, waiting to connect
    AUTOREG_DONE                 = 2, // registered, or permanently given up
};

// Result of a single claim attempt (registration_claim()).
enum claim_result {
    CLAIM_OK       = 0, // credentials stored
    CLAIM_DECLINED = 1, // backend said registered:false
    CLAIM_ERROR    = 2, // transport / parse / storage failure
};

// --- public entry points -----------------------------------------------------

// The FreeRTOS "networking" task body (created in app_main). Never returns.
void networking_task(void *arg);              // FUN_400e1a90

// One tick of the auto-registration state machine (called from the task loop).
void automatic_registration_tick(void);       // FUN_400da854

// Perform one HTTP "claim" round-trip to the AWS claim endpoint.
claim_result registration_claim(void);        // FUN_400da2e0

// Connection-request refcount: bring the wifi-manager + soft-AP up while any
// client is held, tear down at zero.
void conn_req_acquire(void);                   // FUN_400de654
void conn_req_release(void);                   // FUN_400de690
bool soft_ap_is_wanted(void);                  // FUN_400de778
void soft_ap_take_down(void);                  // FUN_400de724
