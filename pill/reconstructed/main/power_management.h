/* power_management.h — RAPT Pill 2.0.0, reverse-engineered (readable C++).
 *
 * Source module recovered as ./main/power_management.c (embedded assert path
 * "…/main/power_management.c", __func__ "power_management_init",
 * "enter_deep_sleep").  Confidence: HIGH for the state machine, enum, and
 * deep-sleep wiring; MEDIUM for some helper names (marked /* inferred *​/).
 *
 * This module owns the device's top-level behaviour: it is the superloop that
 * app_main() falls into (power_management_run), a 5-state power state machine,
 * the battery/ADC fuel-gauge, and deep-sleep entry.  Not original source.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/* The five power states, names recovered verbatim from the
 * "Transitioning to POWER_STATE_*" / "Leaving POWER_STATE_WIFI" log strings.
 * Numeric values are the switch discriminants in power_transition_to(). */
typedef enum {
    POWER_STATE_WIFI        = 0, /* radios up: BLE + wifi-manager (STA), cloud sync   */
    POWER_STATE_ACTIVE      = 1, /* awake, charger/connection servicing, measuring    */
    POWER_STATE_SLEEPY      = 2, /* between measurements; waits, then deep-sleeps      */
    POWER_STATE_IDLE        = 3, /* transient; bounces back to ACTIVE                  */
    POWER_STATE_LOW_BATTERY = 4, /* battery too low: long deep-sleep, radios off       */
} PowerState;

/* ---- lifecycle (called from app_main) ------------------------------------ */
void power_management_init(void);  /* 0x400e5884 — start counter, event group  */
void power_management_run(void);   /* 0x400e4e88 — the superloop (never returns)*/
void power_check_start_battery(void); /* 0x400e5744 — refuse to boot if flat    */

/* ---- state machine ------------------------------------------------------- */
void power_transition_to(PowerState s); /* 0x400e4a50 */
PowerState power_get_state(void);       /* reads g_power_state (0x400d1b18)     */

/* ---- deep sleep ---------------------------------------------------------- */
/* 0x400e4930. use_timer: arm esp_sleep_enable_timer_wakeup for the next wake;
 * gpio_level: ext1 wake GPIO selector; use_ext1: enable ext1 (charger) wake.
 * Restarts via esp_restart() first if the running OTA image is unverified. */
void enter_deep_sleep(bool use_timer, uint32_t gpio_level, bool use_ext1);

/* ---- battery / ADC fuel gauge (ESP_LOG tag "battery") -------------------- */
float battery_voltage_to_charge(float voltage); /* 0x400dcd9c — 0..1, -1 if uncal */
void  battery_set_adc_offset(float raw);         /* 0x400dccc8 */
void  battery_feed_charging_sample(float raw);   /* 0x400dcec0 — auto-cal on charge*/
void  battery_load_calibration(void);            /* 0x400dd034 — from NVS */

/* ---- misc wake/notify ---------------------------------------------------- */
void smart_mode_wake(void);                 /* 0x400e4cc4 — 500 ms connectable peek */
void power_note_reading(const void *rd);    /* 0x400e4754 — "Consecutive flat …" */
void power_request_ap_lock(uint32_t ms);    /* 0x400e4d68 */
