/* power_management.cpp — RAPT Pill 2.0.0, reverse-engineered (readable C++).
 *
 * Recovered from app_2.0.0.bin (ESP32-S0WD, Xtensa).  Original module:
 * ./main/power_management.c.  ESP_LOG tags: "power", "battery", "sleep".
 * Behaviour is faithful to the decompilation; names/constants marked
 * /* inferred *​/ are deduced, not proven.  See docs/RECONSTRUCTION_NOTES.md.
 *
 * ── Cross-module callees referenced here (reconstructed in their own files) ──
 *   on_battery()            FUN_400e69b8  /* inferred: !=0 when on battery, 0 on charger *​/
 *   charger_source()        FUN_400d6ce8  returns 4 = charger present, 3 = …
 *   wake_reason()           FUN_400d4d98  8 = timer deep-sleep wake, 9/3 = …  (esp_sleep_get_wakeup_cause-ish)
 *   ota_image_unverified()  FUN_400e3294  !=1 means running image already verified
 *   ble_connectable_*()     FUN_400ec090/400ec1fc/400ebfa4/400ebffc/400ec6e4/400ec19c/400ec190  (bluetooth.cpp)
 *   wifi_manager_enable()   FUN_400e96b0  (wifi.cpp)       wifi_is_connected() FUN_400e9a14
 *   calibration_run_tick()  FUN_400dde7c  (gravity_calc.cpp)
 *   request_report()        FUN_400e2354  measurement_latest() FUN_400e2404 (measurement.cpp)
 *   registration_*()        FUN_400da7xx/400da814/400da760 (registration.cpp)
 *   ota_in_progress()       FUN_400eae70  soft_ap_clients() >0 holds BLE back
 *
 * ── SDK/FreeRTOS mapping used below ──
 *   millis()        FUN_40092f98   monotonic ms            delay_ms()  FUN_40093438 = vTaskDelay
 *   esp_timer_us()  PTR_FUN_400d02d8 (fills a 64-bit)      gpio_set()  FUN_40105f04 = gpio_set_level
 *   gpio_get()      FUN_40105f9c = gpio_get_level          esp_restart FUN_400d4a58
 *   evgroup_wait()  FUN_40092800 = xEventGroupWaitBits     evgroup_new FUN_40092794
 */
#include "power_management.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"

static const char *TAG = "power";

/* ─────────────────────────── module state ────────────────────────────────
 * These live in DRAM (DAT_400d1b…); offsets kept in comments for traceability.
 * Only the ones whose meaning is clear from the code are named. */
static volatile PowerState g_power_state;          /* 0x400d1b18 */
static volatile uint32_t   g_consecutive_flat;     /* 0x400d1b1c — reset on transition  */
static volatile bool       g_low_battery;          /* 0x400d1b28 — set => go LOW_BATTERY */
static bool                g_gesture_active;        /* 0x400d1b5c — keep-awake/gesture    */
static bool                g_ap_lock_active;        /* 0x400d1bec */
static uint32_t            g_ap_lock_deadline;      /* 0x400d1bf0 */
static bool                g_ble_held_for_reg;      /* 0x400d1bb4 — connectable held back */
static EventGroupHandle_t  g_report_wait_eg;        /* 0x400d1c04 — "report_wait_event_group" */

/* Boot-time knobs loaded from NVS / config (exact provenance in nonvolatile.cpp):
 *   LOW_BATTERY_SLEEP_INTERVAL   deep-sleep µs when flat (0x3f408e72 string)
 *   g_low_battery_charge_thresh  (0x400d1c7c) minimum charge fraction to run
 *   g_gesture_window_ms          (0x400d1bf8) keep-awake window
 *   g_sleepy_measure_period      (0x400d1074) ACTIVE→SLEEPY idle budget        */

PowerState power_get_state(void) { return g_power_state; }

/* ───────────────────────── battery / ADC fuel gauge ──────────────────────
 * A 32-point voltage→charge curve (tables at 0x400d0f2c volts / 0x400d0f34
 * charge) with binary search + linear interpolation.  Returns 0..1, 0 below
 * the low endpoint, 1 above the high endpoint, or -1.0 when uncalibrated. */
float battery_voltage_to_charge(float voltage)           /* 0x400dcd9c */
{
    if (!g_adc_calibrated /*DAT_400d0f04*/) return -1.0f;

    float v = adc_to_voltage(voltage); /* FUN_400dcd44 /* inferred *​/ */
    if (v <= CURVE_V_LOW  /*DAT_400d0f44*/) return 0.0f;
    if (v >= CURVE_V_HIGH /*DAT_400d0f40*/) return 1.0f;

    /* binary search the voltage table (32 entries) */
    uint32_t lo = 0, hi = 0x20;
    for (;;) {
        uint32_t mid;
        do {
            mid = lo;
            if (hi <= lo) {
                if (lo >= 0x20) {           /* search escaped the table */
                    ESP_LOGE(TAG, "voltage_to_charge() binary search result not "
                                  "in range"); /* 0x400d0f30 */
                    return 0.0f;
                }
                /* interpolate between entries lo and lo+1 */
                float c0 = curve_charge[lo], c1 = curve_charge[lo + 1];
                float t  = (v - curve_v[lo + 1]) / (curve_v[lo] - curve_v[lo + 1]);
                float charge = c1 + t * (c0 - c1);
                /* clamp to [curve_min, curve_max] and return */
                return clamp_charge(charge); /* DAT_400d0f48/4c/08d0 helpers */
            }
            lo = (hi + 1 + mid) >> 1;
        } while (v < curve_v[lo]);
        hi = lo - 1;
    }
}

/* Compute/validate the ADC offset (offset = base + raw*scale), bounds-check,
 * and latch it.  Logs and rejects an out-of-bounds result. */
void battery_set_adc_offset(float raw)                   /* 0x400dccc8 */
{
    float offset = ADC_OFF_BASE /*0x400d0f10*/ + raw * ADC_OFF_SCALE /*0x400d0f14*/;
    if (offset < ADC_OFF_MIN || offset > ADC_OFF_MAX) {
        ESP_LOGE(TAG, "Calculated ADC offset out-of-bounds: %g", offset);
        g_adc_calibrated = false;
        return;
    }
    g_adc_offset     = offset;  /* 0x400d0f00 */
    g_adc_calibrated = true;    /* 0x400d0f04 */
}

/* While on the charger, collect raw ADC samples into a 30-deep ring buffer;
 * once ≥24 of 30 look like a stable charging endpoint, persist the value to
 * NVS and recompute the ADC offset.  (This is the self-calibration path.) */
void battery_feed_charging_sample(float raw)             /* 0x400dcec0 */
{
    if (g_filter_primed /*0x400d0f50*/) {
        /* exponential smoothing of the raw reading, log "ADC raw %g. New smoothed…" */
        float smoothed = smooth(raw);
        ESP_LOGI(TAG, "ADC raw %g. New smoothed …");        /* 0x400d0f60 */
        ring_push(smoothed);                                 /* ring of 30 (0x1e) */
    }
    g_last_raw = raw; g_filter_primed = true;

    if (!g_adc_calibrated && ring_full(30)) {
        unsigned below = 0;
        for (int i = 0; i < 30; i++)
            if (sample_below_endpoint(ring[i])) below++;
        if (below > 0x17 /*24*/) {                            /* stable endpoint */
            ESP_LOGI(TAG, "Charging voltage endpoints … (%u)", below);
            ESP_LOGI(TAG, "Raw ADC %g, smoothed …");
            float v = ring_mean();
            nonvolatile_set(KEY_adc_calibration /*0x400d0bf8*/, v); /* FUN_400eb418 */
            battery_set_adc_offset(v);
        }
    }
}

void battery_load_calibration(void)                      /* 0x400dd034 */
{
    g_adc_calibrated = false;
    bool ok = false;
    float v = nonvolatile_get(KEY_adc_calibration, &ok);  /* FUN_400eb350 */
    if (ok) {
        ESP_LOGI(TAG, "Loaded ADC calibration …");
        battery_set_adc_offset(v);
    }
}

/* ───────────────────────────── deep sleep ────────────────────────────────
 * Arms the requested wake sources and enters deep sleep.  If the running OTA
 * image has not yet been marked valid, restarts instead so the bootloader can
 * roll back ("Firmware not verified: restarting for the bootloader…"). */
void enter_deep_sleep(bool use_timer, uint32_t gpio_level, bool use_ext1) /* 0x400e4930 */
{
    prep_sleep();                                   /* FUN_400e4910 */
    if (ota_image_unverified() == 1) {              /* FUN_400e3294 */
        ESP_LOGW(TAG, "Firmware not verified: restarting for the bootloader…");
        delay_ms(0x14);
        esp_restart();                              /* FUN_400d4a58 */
    }
    phy_close_rf();                                 /* FUN_400e6f50 */
    esp_sleep_pd_config(/*…*/ 2, 1);                /* FUN_400d6d38 */
    if (wake_pin_asserted() /*FUN_400dd8bc*/) note_wake_pin(); /* FUN_400e4858 */

    if (use_timer) {
        uint64_t secs = seconds_until_next_wake();  /* FUN_400e12c0 /* inferred *​/ */
        if (clamp_wanted() && secs > wake_cap())    /* FUN_400e487c / FUN_400e6ab4 */
            secs = wake_cap();
        esp_err_t e = esp_sleep_enable_timer_wakeup(secs * 1000000ULL); /* FUN_400d6ae8 */
        if (e) ESP_ERROR_CHECK_fail(e, "enter_deep_sleep",
                 "esp_sleep_enable_timer_wakeup(((uint64_t)seconds)*1000000ULL)");
    } else {
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER); /* FUN_400d6a00(4) */
    }

    if (use_ext1) {
        esp_err_t e = esp_sleep_enable_ext1_wakeup((gpio_level << 2) | 8,
                                 ESP_EXT1_WAKEUP_ANY_HIGH);        /* FUN_400d6cc0 */
        if (e) ESP_ERROR_CHECK_fail(e, "enter_deep_sleep",
                 "esp_sleep_enable_ext1_wakeup(gpio_mask, ESP_EXT1_WAKEUP_ANY_HIGH)");
    } else {
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_EXT1);    /* FUN_400d6a00(3) */
    }

    /* LOW_BATTERY path re-arms the timer with the long interval instead */
    if (g_power_state == POWER_STATE_LOW_BATTERY &&
        esp_sleep_enable_timer_wakeup(LOW_BATTERY_SLEEP_INTERVAL)) /* 0x3f408e72 */
        { /* error-checked above */ }

    pre_sleep_gpio_hold(0xc);          /* FUN_401065e8 */
    if (esp_deep_sleep_try_to_start() == 0 /*FUN_40082980*/ && should_hold()) {
        gpio_hold_apply();  gpio_deep_sleep_hold_en(1);
    } else {
        gpio_deep_sleep_hold_dis();
    }
    /* esp_deep_sleep_start() does not return */
}

/* ───────────────────────── state transition ──────────────────────────────
 * The single entry point that moves g_power_state.  Each branch logs
 * "Transitioning to POWER_STATE_…" (strings 0x3f408f24/8fd9/9031/9064/9095). */
void power_transition_to(PowerState target)              /* 0x400e4a50 */
{
    PowerState cur = g_power_state;
    if (cur == target) { ESP_LOGW(TAG, "called transition to the current state %d", cur); return; }

    /* Leaving WIFI: release the wifi-manager and tear down the AP/STA session. */
    if (cur == POWER_STATE_WIFI) {
        if (g_ble_connectable_wifi /*0x400d1b84*/) { ble_stop_wifi_adv(); ble_connectable(2, false); registration_tls_free(); }
        g_ble_connectable_wifi = false;
        ESP_LOGI(TAG, "Leaving POWER_STATE_WIFI, releasing wifi manager");
        wifi_manager_enable(1 /*release*/);                /* FUN_400e96b0 */
        if (g_reg_resolved /*0x400d1b8c*/) { registration_on_wifi_release(); g_reg_resolved = false; }
        else                               registration_keep();
        ESP_LOGI(TAG, "Minimum free heap size: %u bytes", esp_get_minimum_free_heap_size());
    }

    switch (target) {
    case POWER_STATE_WIFI:   /* 0 */
        ESP_LOGI(TAG, "Transitioning to POWER_STATE_WIFI...");
        request_report();                                  /* FUN_400ded58 */
        g_wifi_enter_ms = millis();                        /* 0x400d1b98 */
        g_in_wifi = true;
        wifi_manager_enable(!on_battery());                /* bring radios up (STA only) */
        ESP_LOGI(TAG, "Starting BLE and wifi manager (STA only)");
        registration_on_wifi_enter();                      /* FUN_400de654 */
        /* open BLE connectable unless registration/OTA holds it back */
        if (!g_ap_has_client && !on_battery() && registration_wants_ble())
            g_ble_connectable_wifi = true;
        else { g_ble_connectable_wifi = false; registration_keep(); }
        if (on_battery()) registration_battery_hint();     /* FUN_400da760 */
        if (ota_in_progress()) ESP_LOGI(TAG, "BLE connectable held back …");
        else if (g_ble_connectable_wifi) ble_start_connectable(); /* FUN_400e4a38 */
        else                             ble_connectable(0, 0);   /* FUN_400ebffc */
        if (!wifi_is_connected()) wifi_kick();             /* FUN_400ea0c4 */
        break;

    case POWER_STATE_ACTIVE: /* 1 */
        ESP_LOGI(TAG, "Transitioning to POWER_STATE_ACTIVE...");
        gpio_set(0x12, 1);
        active_enter();                                    /* func_0x400e4a18 */
        if (!wake_pin_asserted() && !ble_is_connected()) {
            if (ble_connectable_active()) ESP_LOGI(TAG, "Stopping BLE connectable …");
            ble_connectable(2, false);
        }
        g_gesture_active = false;
        g_active_enter_ms = millis();
        request_report();
        break;

    case POWER_STATE_SLEEPY: /* 2 */
        ESP_LOGI(TAG, "Transitioning to POWER_STATE_SLEEPY...");
        break;

    case POWER_STATE_IDLE:   /* 3 */
        ESP_LOGI(TAG, "Transitioning to POWER_STATE_IDLE...");
        break;

    case POWER_STATE_LOW_BATTERY: /* 4 */
        ESP_LOGI(TAG, "Transitioning to POWER_STATE_LOW_BATTERY...");
        g_power_state = POWER_STATE_LOW_BATTERY;
        low_battery_prep();                                /* FUN_400e0694 */
        enter_deep_sleep(false, 0, false);                 /* radios already off */
        /* loops back through IDLE log then deep-sleeps; see decompilation */
        break;
    }
    g_power_state = target;
}

/* ───────────────────── boot-time battery gate ────────────────────────────
 * On a fresh (non-deep-sleep) start, refuse to run if the battery is below the
 * configured charge threshold — go straight to LOW_BATTERY deep sleep. */
void power_check_start_battery(void)                     /* 0x400e5744 */
{
    if (wake_reason() == 8) return;                      /* woke from timer: skip */
    if (!gpio_get(0x17)) return;                         /* 0x17 = charger sense */
    if (ota_image_unverified() == 1) return;

    float adc[9];
    if (read_battery_adc(adc) != 0) return;              /* FUN_400e093c */

    float charge = battery_voltage_to_charge(adc[0]);
    if (charge >= 0.0f) {
        if (charge >= g_low_battery_charge_thresh)       /* 0x400d1c7c */
            { ESP_LOGI(TAG, "Start: battery %.0f %% (%.2f V) …"); return; }
        ESP_LOGW(TAG, "Start: battery %.0f %% (%.2f V), under %.0f %%: …");
    } else {                                             /* uncalibrated: use raw ADC */
        if (adc[0] <= g_adc_low_threshold) { ESP_LOGI(TAG, "Start: battery ADC … ok"); return; }
        ESP_LOGW(TAG, "Start: battery ADC … low");
    }
    power_transition_to(POWER_STATE_LOW_BATTERY);        /* => deep sleep */
}

/* ───────────────────────── init (from app_main) ──────────────────────────
 * Counts consecutive brown-out-prone starts, warns if ≥10 in a row, then
 * creates the "report_wait_event_group" and arms measurement. */
void power_management_init(void)                         /* 0x400e5884 */
{
    if (wake_reason() != 8) { g_power_state = POWER_STATE_ACTIVE; g_consecutive_flat = 0; }
    if (wake_reason() == 9) { on_brownout(); power_transition_to(POWER_STATE_LOW_BATTERY); }

    uint32_t starts = nonvolatile_get_u32(KEY_starts_in_a_row /*0x400d1b60*/, 0);
    int wr = wake_reason();
    if (wr != 8 && wr != 3) {
        starts++;
        if (starts >= 10) {
            ESP_LOGW(TAG, "%u starts in a row did … (resetting)", starts);
            nonvolatile_set_u32(KEY_starts_in_a_row, 0);
            on_brownout();
            power_transition_to(POWER_STATE_LOW_BATTERY);
        } else if (starts > 1) {
            ESP_LOGW(TAG, "Start %u of %d in a row that may not last", starts, 10);
        }
        nonvolatile_set_u32(KEY_starts_in_a_row, starts);
    }

    g_gesture_active = (starts != 0);
    measurement_set_sink(report_measurement_cb /*0x400d1c88*/); /* FUN_400e2430 */
    g_report_wait_eg = xEventGroupCreate();                      /* FUN_40092794 */
    configASSERT(g_report_wait_eg); /* "report_wait_event_group" @ power_management.c:0x837 */
    start_charge_monitor(); start_misc();                        /* FUN_400deda0 / FUN_400dd6b8 */
}

/* ─────────────── smaller helpers (documented, not fully transcribed) ──────
 * smart_mode_wake()      0x400e4cc4  — advertise connectable 500 ms; if a phone
 *                        connects within the window, ESP_LOGI "Smart mode wake:
 *                        phone …" and transition_to(ACTIVE).
 * power_note_reading()   0x400e4754  — in SLEEPY, if the gravity reading sits in
 *                        the flat band [0x400d1b38,0x400d1b34], increments
 *                        g_consecutive_flat ("Consecutive flat readings: %u");
 *                        else resets it.  Also sets g_low_battery when the fuel
 *                        gauge/raw ADC is under threshold while on battery.
 * power_request_ap_lock()0x400e4d68  — reserve a wifi session window; logs
 *                        "AP lock requested outside POWER_STATE_WIFI …".
 * poll_usb_charger()     0x400e6ac0  — GPIO 0x17 edge → "USB charger connected/
 *                        disconnected" (tag "state").
 * charger_debounce()     0x400e47fc  — 6000 ms debounce of the charger line.
 * seconds_until_next_wake() 0x400e12c0 — 64-bit (now vs next-wake target) under a lock.
 * Remaining tiny accessors on DAT_400d1b…: g_* getters/setters whose exact
 * field semantics are not individually recoverable; preserved by address in
 * the superloop below. */

/* ─────────────────────────── the superloop ───────────────────────────────
 * app_main() tail-calls this; it never returns.  Reconstructed as a per-state
 * dispatch — the decompiled body (0x400e4e88) is one large do/while that polls
 * every subsystem each ~2 ms tick and drives transitions.  The important,
 * user-visible behaviours and their exact log strings are preserved; the dense
 * flag bookkeeping is summarised. */
void power_management_run(void)                          /* 0x400e4e88 */
{
    if (ota_image_unverified() == 1) {                   /* just-flashed image path */
        poll_usb_charger();
        g_ble_connectable_wifi = on_battery();
        power_transition_to(POWER_STATE_WIFI);           /* sync with cloud to confirm */
        power_request_ap_lock(g_post_ota_window /*0x400d1930*/);
    }

    for (;;) {                                           /* ~2 ms tick */
        poll_usb_charger();                              /* FUN_400e6ac0 */

        if (g_gesture_active && millis() > 2999) prep_sleep(); /* keep-awake budget */
        calibration_run_tick();                          /* FUN_400dde7c (gravity_calc) */

        /* ── registration / BLE-connectable gating ──
         * While automatic registration resolves (or an OTA / soft-AP client is
         * active) BLE connectable is held back, then re-opened: */
        //  "Charger connected: BLE connectable held back while automatic registration is resolved"
        //  "Automatic registration resolved: opening BLE connectable window"
        //  "Automatic registration resolved, BLE connectable waiting for %s"  ("the OTA to finish"/"the soft AP client to leave")
        //  "BLE connectable taken down while automatic registration is resolved"
        //  "AP-locked session: re-arming BLE connectable" / "AP lock expired"
        service_registration_and_ble_gating();

        /* ── gesture/keep-awake session open ── */
        if (consume_flag(&g_gesture_request /*0x400d1bfc*/)) {
            if (g_power_state != POWER_STATE_WIFI) { g_ble_connectable_wifi = true; power_transition_to(POWER_STATE_WIFI); }
            power_request_ap_lock(g_gesture_window_ms /*0x400d1bf8*/);
            ESP_LOGI(TAG, "Gesture session open for %u s", g_gesture_window_ms / 1000);
        }

        switch (g_power_state) {
        case POWER_STATE_WIFI:
            /* measure, feed the battery charging filter, sync cloud, and decide
             * when to leave WIFI for ACTIVE (idle budget 0x400d1074 elapsed). */
            service_wifi_state();
            if (ready_to_leave_wifi()) power_transition_to(POWER_STATE_ACTIVE);
            break;

        case POWER_STATE_ACTIVE:
            /* service charger/connection; after the active window with
             * seconds_until_next_wake()>0, drop to SLEEPY. */
            service_active_state();
            if (active_window_expired() && !on_battery() && seconds_until_next_wake() > 0)
                power_transition_to(POWER_STATE_SLEEPY);
            break;

        case POWER_STATE_SLEEPY:
            if (charger_source() == 4) {                 /* still on charger */
                if (seconds_until_next_wake() == 0) {
                    /* wait for the measurement/report to complete, up to ~5.7 s */
                    EventBits_t b = xEventGroupWaitBits(g_report_wait_eg, 1, pdTRUE, pdFALSE,
                                                        pdMS_TO_TICKS(0x1644));
                    if (!(b & 1)) ESP_LOGE(TAG, "Timed out waiting for the report");
                    else {
                        if (g_consecutive_flat > 1) power_transition_to(POWER_STATE_IDLE);
                        if (pending_report()) smart_mode_wake();
                    }
                } else if (pending_report() && recent_reading()) smart_mode_wake();
            } else if (charger_source() == 3) {
                power_transition_to(POWER_STATE_ACTIVE);
            }
            if (g_power_state == POWER_STATE_SLEEPY) {   /* otherwise deep-sleep now */
                ESP_LOGI(TAG, "Going back to deep sleep");
                enter_deep_sleep(true, true, false);     /* timer + ext1, then re-enter */
            }
            break;

        case POWER_STATE_IDLE:
            power_transition_to(POWER_STATE_ACTIVE);
            break;

        case POWER_STATE_LOW_BATTERY:
            break; /* unreachable: transition_to(LOW_BATTERY) deep-sleeps */
        }

        if (on_battery() && g_low_battery) power_transition_to(POWER_STATE_LOW_BATTERY);

        update_charge_led();                             /* FUN_400e6d24 */
        housekeeping();                                  /* FUN_400e6d5c */
        delay_ms(2);                                     /* vTaskDelay(2) */
    }
}
