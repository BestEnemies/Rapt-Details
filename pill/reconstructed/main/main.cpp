// =============================================================================
// main.cpp  —  RAPT Pill Hydrometer, firmware 2.0.0  (RECONSTRUCTED)
//
// Reverse-engineered from app_2.0.0.bin (ESP-IDF, ESP32-S0WD, Xtensa LE).
// Not original source. See reconstructed/docs/RECONSTRUCTION_NOTES.md.
// Recovered source name: "main.cpp" (embedded __func__ "void app_main()").
// Confidence: HIGH for the boot/init sequence and task model.
//
// Entry point app_main() @ 0x400e1c78. All other addresses are the Ghidra
// FUN_<addr> symbols from that image.
// =============================================================================
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#include "driver/gpio.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_tls.h"
#include "esp_ota_ops.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

// ---- cross-module init entry points (reconstructed in their own files) ------
extern "C" {
bool  nonvolatile_init(void);                 // nonvolatile.cpp  (FUN_0x400eb07c)
void  gravity_calc_init(void);                // gravity_calc.cpp (FUN_0x400df878)
void  calibration_run_init(void);             // gravity_calc.cpp (FUN_0x400ddd30)
void  automation_load_stored(void);           // gravity_calc.cpp (FUN_0x400d96a8)
void  connectivity_request_init(void);        // registration/connectivity_request (FUN_0x400de61c)
void  measurement_init(void);                 // measurement.cpp  (FUN_0x400e2254)
void  power_management_init(void);            // power_management (FUN_0x400e5744)
void  bluetooth_init(void);                   // bluetooth        (FUN_0x400ec254)
void  aws_iot_init(void);                     // aws_shadow       (FUN_0x400f0624)
void  wifi_manager_init(void);                // esp32-wifi-manager (FUN_0x400e96ec)
void  firmware_info_init(void);               // ota/main         (FUN_0x400e318c)
int   ota_mark_boot_and_report_rollback(void);// ota              (FUN_0x400e1360 / FUN_0x400e3790)
void  power_management_run(void);             // power_management (FUN_0x400e4e88) — never returns
uint32_t firmware_version_code(void);         // (FUN_0x400e6998)
}

static const char *TAG = "main";              // PTR_DAT_400d14f0

// RTC-retained state (survives deep sleep). Zeroed on a cold/non-deepsleep boot
// by init_rtc_retained_state(). Backs the smart-mode / measurement cadence.
// (Exact fields live in power_management; here we only clear them.) /* inferred */

// -----------------------------------------------------------------------------
// FUN_0x400e0c68 — init_rtc_retained_state(): on a non-deep-sleep reset, clear
// the RTC-retained counters, then install the brownout/boot hook.
// -----------------------------------------------------------------------------
static void init_rtc_retained_state(void)
{
    if (esp_reset_reason() != ESP_RST_DEEPSLEEP) {   // FUN_0x400d4d98 != 8
        rtc_retained.field_a = 0;   // DAT_400d14a0  /* inferred names */
        rtc_retained.field_b = 0;   // DAT_400d14a4
        rtc_retained.field_c = 0;   // DAT_400d149c
        rtc_retained.flag    = 0;   // DAT_400d14a8
        rtc_retained.field_d = 0;   // DAT_400d14ac
        rtc_retained.field_e = 0;   // DAT_400d14b0
    }
    boot_hook_handle = install_boot_hook(boot_hook_fn);  // PTR_FUN_400d14bc
}

// -----------------------------------------------------------------------------
// FUN_0x400d6ce8 — power_source(): decode the charge/USB status from the
// charger IC strap + RTC GPIO register. Returns a small enum code:
//   0 = none, 2/3/5/6/7 = various charge states, 4 = charger/USB connected.
// app_main treats "== 4" as "charger connected".  /* inferred codes */
// -----------------------------------------------------------------------------
static int power_source(void)
{
    int strap = read_charger_strap(0);           // (*DAT_400d0158)(0)
    if (strap != 5 && *charger_present == 0)      // DAT_400d0490
        return 0;
    uint32_t r = *rtc_charge_status_reg;          // DAT_400d0494
    if (r & 8)   return 4;
    if (r & 4)   return 7;
    if ((r >> 6) & 3) return 8;
    if (r & 1)   return 2;
    if (r & 2)   return 3;
    if (r & 0x100)    return 5;
    if ((r >> 9) & 1) return 6;
    return 0;
}

// -----------------------------------------------------------------------------
// FUN_0x400e96c0 — set_device_name(): copy up to 31 chars ("RAPT-Pill-XXXXXX")
// into the global device-name buffer and push it to the BLE GAP name if set.
// -----------------------------------------------------------------------------
static void set_device_name(const char *name)
{
    if (!name) return;
    strncpy(device_name, name, 0x1f);             // DAT_400d20b8
    device_name[0x1f] = '\0';
    if (ble_gap_name_setter)                        // DAT_400d20bc
        ble_gap_name_setter(ble_gap_name_setter, device_name);  // FUN_0x4011f4ac
}

// register-callback helpers (small fixed arrays filled before the net task)
static void register_connect_handler(void (*cb)(void)) {   // FUN_0x400e6934
    if (connect_handler_count < 4)
        connect_handlers[connect_handler_count++] = cb;
}
static void set_wifi_config_handler(void *cb){ wifi_cfg_handler = cb; } // FUN_0x400e9688
static void set_ble_ready_handler(void *cb)  { ble_ready_handler = cb; } // FUN_0x400eaf0c

// =============================================================================
// FUN_0x400e1c78 — app_main(): the single ESP-IDF entry point.
//
// Boot flow:
//   1. Decide initial power/charger state and whether battery is healthy.
//   2. Quiet the noisy SDK log tags (wifi, wifi_init, …) to WARN.
//   3. Bring up GPIO, NVS, the global TLS CA store, OTA/firmware info.
//   4. Initialise every application subsystem, then hand control to the
//      power-management run loop (which sleeps/wakes the device forever).
// =============================================================================
extern "C" void app_main(void)
{
    // ---- 1. power source + battery sanity --------------------------------
    bool connectable;
    if (esp_reset_reason() == ESP_RST_DEEPSLEEP) {            // FUN_0x400d4d98 == 8
        // Woke from deep sleep: be connectable only if on the charger and the
        // next scheduled wake is still >= 3 s away.
        connectable = false;
        if (power_source() == 4)                              // charger connected
            connectable = (seconds_until_next_wake() >= 3);   // FUN_0x400e12c0 (power_management) /* inferred use */
        *charger_connected = connectable;                     // DAT_400d1518
    } else {
        // Cold boot: assume connectable window open, clear advertising timers.
        adv_window_ms = 0x708;   // 1800  PTR_DAT_400d14fc  /* inferred */
        adv_timer_a = adv_timer_b = 0;                        // PTR_DAT_400d150c
        adv_timer_c = adv_timer_d = 0;                        // PTR_DAT_400d14e8
    }

    init_rtc_retained_state();                                // FUN_0x400e0c68
    set_charger_inverted(*charger_connected ^ 1);             // FUN_0x400e0ca8

    // ---- 2. quiet the SDK log tags ---------------------------------------
    esp_log_level_set("wifi",      ESP_LOG_WARN);             // FUN_0x400d579c
    esp_log_level_set("wifi_init", ESP_LOG_WARN);
    esp_log_level_set("wifi_mgr",  ESP_LOG_WARN);             // PTR_DAT_400d1594 /* inferred tag */
    ESP_LOGI(TAG, "Hello world!");

    connectivity_request_init();                              // FUN_0x400de61c

    // ---- 3. GPIO bring-up (charger/LED/strap pins) -----------------------
    gpio_config_t io = {};
    io.pin_bit_mask = 0x40000;   // GPIO18 /* inferred */
    io.mode         = GPIO_MODE_OUTPUT;
    ESP_ERROR_CHECK(gpio_config(&io));                        // FUN_0x40106114
    gpio_set_level((gpio_num_t)18, power_source() == 4);      // FUN_0x40105f04

    io = {};
    io.pin_bit_mask = 0x800000;  // GPIO23 /* inferred */
    io.mode         = GPIO_MODE_INPUT;
    io.pull_up_en   = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK(gpio_config(&io));

    io = {};
    io.pin_bit_mask = 0x20;      // GPIO5 /* inferred */
    io.mode         = GPIO_MODE_OUTPUT;
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_set_level((gpio_num_t)5, 0);
    gpio_hold_dis((gpio_num_t)34);  // FUN_0x401063c0(0x22) /* inferred */

    io = {};
    io.intr_type = GPIO_INTR_DISABLE;  // 4th config, interrupt pin /* inferred */
    ESP_ERROR_CHECK(gpio_config(&io));

    // ---- NVS + application NVS layer -------------------------------------
    ESP_ERROR_CHECK(nvs_flash_init());                        // FUN_0x40109eb0
    nonvolatile_init();                                       // FUN_0x400eb07c
    firmware_info_init();                                     // FUN_0x400e6ba4 (sub-inits)
    connectivity_request_init();                              // FUN_0x400dd034 /* dup lock init */
    power_management_init();                                  // FUN_0x400e5884 /* pre-init */
    gravity_calc_init();                                      // FUN_0x400df878
    calibration_run_init();                                   // FUN_0x400ddd30
    automation_load_stored();                                 // FUN_0x400d96a8
    firmware_info_log();                                      // FUN_0x400e1c04

    // telemetry intervals cached from NVS (wifi/ble)
    telemetry_interval_wifi = nonvolatile_get_u32(KEY_telemetry_interval_wifi, 0); // FUN_0x400eb1d8
    telemetry_interval_ble  = nonvolatile_get_u32(KEY_telemetry_interval_ble, 0);

    networking_event_group = xEventGroupCreate();             // FUN_0x40092794
    if (networking_event_group == NULL)
        __assert_func("main.cpp", 0x40e, "void app_main()", "networking_event_group");

    // ---- global TLS CA store (for api.rapt.io + AWS) ---------------------
    ESP_ERROR_CHECK(esp_tls_init_global_ca_store());          // FUN_0x40121818
    ESP_ERROR_CHECK(esp_tls_set_global_ca_store(ca_store_pem, ca_store_pem_len)); // FUN_0x40121824

    // ---- OTA / firmware info + rollback reporting ------------------------
    firmware_info_init();                                     // FUN_0x400e318c (OTA boot offset log)
    if (esp_reset_reason() != ESP_RST_DEEPSLEEP) {
        bool rolled_back = false;
        int verdict = ota_rollback_pending(&verdict_buf, &rolled_back);  // FUN_0x400e3790
        if (verdict) {
            ESP_LOGI(TAG, "Firmware update failure not told yet (%s)",
                     rolled_back ? ", rolled back" : "");
            ota_mark_boot_and_report_rollback();              // FUN_0x400e1360
        }
    }

    // Persist the BLE service version into NVS if absent.
    char ver_buf[0x30];
    if (nonvolatile_get_blob(nvs_ble_ver_key, ver_buf, 0x30) == 0) {  // FUN_0x400eb508
        uint32_t v = firmware_version_code();                  // FUN_0x400e6998
        ESP_LOGI(TAG, "No BLE service version stored yet; using %lu", (unsigned long)v);
        nonvolatile_set(nvs_ble_ver_key, firmware_version_code()); // FUN_0x400eb63c
    }

    // ---- device name "RAPT-Pill-XXXXXX" from the STA MAC -----------------
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);                      // FUN_0x401099b0
    char name[0x20];
    snprintf(name, sizeof(name), "RAPT-Pill-%02X%02X%02X",    // FUN_0x400de898
             mac[3], mac[4], mac[5]);
    set_device_name(name);                                    // FUN_0x400e96c0

    // ---- remaining subsystem init + handler registration ----------------
    wifi_manager_init();                                      // FUN_0x400e96ec (esp32-wifi-manager)
    set_wifi_config_handler((void*)wifi_config_cb);           // FUN_0x400e9688
    set_ble_ready_handler((void*)ble_ready_cb);               // FUN_0x400eaf0c
    measurement_init();                                       // FUN_0x400e2254
    power_management_init();                                  // FUN_0x400e5744
    bluetooth_init();                                         // FUN_0x400ec254
    aws_iot_init();                                           // FUN_0x400f0624
    register_connect_handler(on_network_connected);           // FUN_0x400e6934

    // ---- the networking task --------------------------------------------
    if (xTaskCreate(networking_task, "networking", 0x3400,    // FUN_0x40094158
                    NULL, 0, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Could not create networking task");
    }

    gpio_set_level((gpio_num_t)18, 1);                        // charger LED/enable on
    ESP_LOGI(TAG, "Minimum free heap size: %lu bytes",
             (unsigned long)esp_get_minimum_free_heap_size());

    // Hand off to the power-management run loop. Never returns: it measures,
    // reports, and deep-sleeps the Pill on its cadence.
    power_management_run();                                   // FUN_0x400e4e88
}

// =============================================================================
// Supporting helpers that live in main's translation unit
// =============================================================================

// FUN_0x400d579c — esp_log_level_set(tag, level): thin wrapper around the SDK
// (lock, hash lookup, set-or-insert). Reproduced here for readability.
// FUN_0x400de898 — snprintf(buf, size, fmt, ...): wraps vsnprintf (PTR_FUN_400d120c).

// FUN_0x400e318c — firmware_info_init(): log the OTA boot/running partition
// offsets and current firmware version, cache the running partition's app
// description, and record OTA state. Uses esp_ota_get_boot_partition
// (FUN_0x40106b18), esp_ota_get_running_partition (FUN_0x40106794),
// esp_ota_get_partition_description (FUN_0x40106be0),
// esp_ota_get_state_partition (FUN_0x40106da4).
//   "OTA boot offset 0x%08lx, running offset 0x%08lx"
//   "Current firmware version %s"

// FUN_0x400e2a84 — ota_partition_labels(): iterate the partition table building
// "<label> <crc32>" strings for the running/boot app slots (helper for
// firmware_info_init). Uses the esp_partition iterator (FUN_0x40106b8c/da4/be0).

// FUN_0x400f0624 — aws_iot_init(): create the cloud primitives and start the
// IoT task. Recovered from assertion strings:
//   mirror_mutex, tls_context_lock          (recursive mutexes)
//   iot_event_group                         (xEventGroupCreate)
//   clear_registration_done                 (binary semaphore)
//   alert_queue           = xQueueCreate(8, 0x58)
//   telemetry_result_queue= xQueueCreate(1, 1)
//   xTaskCreate(iot_task, "iot", 8192, NULL, <prio>, &iot_task_handle)
// Loads the refresh token / AWS client cert from NVS before starting the task.
// (Reconstructed in detail in aws_shadow.cpp.)
