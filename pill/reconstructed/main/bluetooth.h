// =============================================================================
// bluetooth.h — RAPT Pill 2.0.0 BLE/GATT protocol (RECONSTRUCTED)
// Reverse-engineered from app_2.0.0.bin. Not original source.
// =============================================================================
#pragma once
#include <stdint.h>

// ---- GATT UUIDs -------------------------------------------------------------
// Base 128-bit UUID: 52415054-0a06-9215-08bf-f2b01884__00  ("52415054" = "RAPT")
// The two bytes marked __ select service/characteristic.
//
//   Service 0x8400xx00 "devinfo" : chars 0004/0005/0006 (rw), 0007 (write)
//   Service 0x8401xx00 "wifi"    : chars 0101/0102 (rw), 0103 (write),
//                                   0104 (rw+notify), 0105 (read+notify)
//   Service 0x8402xx00 "reg"     : char  0201 (rw), 0202/0203 (write)
//
// CONFIRMED on the live device: the single control characteristic that accepts
// JSON is  …840006  (value handle 0x14); it accepts BOTH {"cmd":…} and
// {"desired":{…}}, routed by the top-level key.

// ---- sub-characteristic IDs (the internal routing byte) ---------------------
enum ble_sub_char {
    SUB_INFO            = 0x01,  // read: device-info JSON
    SUB_DESIRED         = 0x0B,  // write {"desired":…}; read: shadow desired
    SUB_AUTH            = 0x0C,  // write: owner-auth secret
    SUB_SHADOW_REPORTED = 0x0D,  // read: shadow reported
    SUB_COMMAND         = 0x0E,  // write {"cmd":…,"args":…}
    SUB_EXPORT          = 0x0F,  // read: export data
};

// ---- ATT error codes returned by the handlers -------------------------------
enum att_err {
    ATT_OK                 = 0x00,
    ATT_INVALID_HANDLE     = 0x02,
    ATT_WRITE_NOT_PERMITTED= 0x03,
    ATT_READ_NOT_PERMITTED = 0x06,  // used here for "unknown sub-char / bad write"
    ATT_INSUFFICIENT_AUTH  = 0x08,  // owner-auth required but peer not authed
    ATT_CMD_FAILED         = 0x0E,  // command dispatch returned failure
    ATT_UNLIKELY_ERROR     = 0x0E,
    ATT_INSUFFICIENT_RES   = 0x11,  // out of memory
    ATT_WRITE_REQ_REJECTED = 0x13,  // malformed JSON / missing field
};

// ---- Direct command set (dispatched from the "cmd" JSON string) -------------
// See command_dispatch() in bluetooth.cpp (FUN_0x400e63e8).
//   keep_awake, report_now, upload_log,
//   start_automation, stop_automation,
//   start_calibration_water, start_calibration_wort, cancel_calibration,
//   clear_calibration, set_dev_options, set_beta_updates,
//   clear_wifi, clear_registration, factory_reset, export

// ---- owner-auth state (RAM only; zeroed at boot; no NVS copy) ----------------
struct owner_auth_state {
    bool     enabled;        // DAT_0x400d2468  — if false, is_authenticated()==true
    char     secret[0x40];   // DAT_0x400d2484
    bool     locked_out;     // DAT_0x400d2474
    uint32_t lockout_until;  // DAT_0x400d2478 (ms)
    uint32_t wrong_tries;    // *DAT_0x400d2480
};

void bluetooth_init(void);                 // FUN_0x400ec254
bool owner_is_authenticated(uint16_t conn);// FUN_0x400ec720
int  command_dispatch(const char *cmd, const void *args_json); // FUN_0x400e63e8
