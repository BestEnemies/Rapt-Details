// wifi.h — RAPT Pill 2.0.0, reverse-engineered (readable reconstruction)
//
// RAPT-specific glue around the vendored esp32-wifi-manager library
// (./libs/RAPT.Firmware.Common/esp32-wifi-manager). The manager itself
// (its task, HTTP server, event loop) is an external library and is NOT
// reimplemented here; only the RAPT overrides/helpers are reconstructed.
//
// Provenance: app_2.0.0.bin, log tag "wifi_manager".  See docs/RECONSTRUCTION_NOTES.md.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// esp32-wifi-manager stores the STA config as a wifi_config_t blob in NVS
// namespace "espwifimgr", keys "ssid" (32 B) and "password" (64 B).
#define WIFIMGR_NVS_NAMESPACE "espwifimgr"
#define WIFIMGR_NVS_KEY_SSID  "ssid"
#define WIFIMGR_NVS_KEY_PASS  "password"

// wifi_manager message codes passed to wifi_manager_send_message().
// Only the values observed in this image are named; the enum is the library's.
enum wifi_manager_message {
    WM_ORDER_STOP_AP        = 0x0d, // FUN_400eaec4 "Stopping access point"
    WM_ORDER_START_WIFI_SCAN= 0x0e, // used by wifi_manager_destroy() path /* inferred */
};

// Persist the current wifi_config_t SSID/password to NVS ("espwifimgr").
// Reconstruction of the RAPT copy of wifi_manager_save_sta_config().
void wifi_manager_save_sta_config(void);

// Load the saved STA config from NVS into the manager's wifi_config_t.
// Returns true if a non-empty SSID was present.
bool wifi_manager_fetch_wifi_sta_config(void);

// Post a message to the wifi_manager task queue. Returns pdTRUE/pdFALSE.
BaseType_t wifi_manager_send_message(int code, void *param);

// Tear the manager fully down (free buffers, delete tasks/queues/event group).
void wifi_manager_destroy(void);

// Ask the manager to stop the soft-AP.
void wifi_manager_stop_ap(void);

// JSON/config buffer mutex (guards the shared wifi_config_t).
bool wifi_manager_lock_json_buffer(TickType_t ticks); // FUN_400eb044
void wifi_manager_unlock_json_buffer(void);           // FUN_400eb064
