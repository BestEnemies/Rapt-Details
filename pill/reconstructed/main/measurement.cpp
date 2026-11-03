// measurement.cpp — sensor acquisition + telemetry assembly (reverse-engineered)
//
// Reconstructed from app_2.0.0.bin. See measurement.h. The TMP1075 I2C driver,
// task/queue model and NVS key registry are recovered at High confidence; the
// report builder (FUN_0x400db9d0) is represented structurally (Medium).
//
// I2C call map (ESP-IDF legacy driver), resolved from the decompilation:
//   i2c_cmd_link_create  FUN_0x4010ed58   i2c_master_start      FUN_0x4010ed98
//   i2c_master_write_byte FUN_0x4010ee30  i2c_master_read_byte  FUN_0x4010ef40
//   i2c_master_stop      FUN_0x4010ede0   i2c_master_cmd_begin  FUN_0x4010f02c
//   i2c_cmd_link_delete  FUN_0x4010ed70   esp_err_to_name       FUN_0x40109304

#include "measurement.h"
#include "nonvolatile.h"
#include "esp_log.h"
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char* TAG = "measure";

// TMP1075 digital temperature sensor (I2C addr 0x48; 0x90 write / 0x91 read).
#define TMP1075_ADDR_W   0x90
#define TMP1075_ADDR_R   0x91
#define TMP1075_REG_TEMP 0x00
#define TMP1075_REG_CFGR 0x01
#define TMP1075_REG_DIEID 0x0F      // high byte reads 0x75 on a genuine part

// Queues / task handles (DAT_400d15fc start, 400d162c finished, 400d1628 pending).
static QueueHandle_t s_start_q;      // measurement_start
static QueueHandle_t s_finished_q;   // measurement_finished

// ---- TMP1075 driver -------------------------------------------------------

// tmp1075_read_reg @ FUN_0x400e7cfc — write register pointer, read 2 bytes big-endian.
static esp_err_t tmp1075_read_reg(uint8_t reg, uint16_t* out)
{
    uint8_t hi, lo;
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, TMP1075_ADDR_W, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, TMP1075_ADDR_R, true);
    i2c_master_read_byte(cmd, &hi, I2C_MASTER_ACK);
    i2c_master_read_byte(cmd, &lo, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(I2C_NUM_0, cmd, 100 / portTICK_PERIOD_MS);
    i2c_cmd_link_delete(cmd);
    /* release the shared I2C bus lock — FUN_0x40093438(1) */
    *out = (uint16_t)((hi << 8) | lo);
    return err;
}

// tmp1075_write_reg @ FUN_0x400e7d78 — write a 16-bit register value.
static esp_err_t tmp1075_write_reg(uint8_t reg, uint16_t value)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, TMP1075_ADDR_W, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write_byte(cmd, (value >> 8) & 0xff, true);
    i2c_master_write_byte(cmd, value & 0xff, true);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(I2C_NUM_0, cmd, 100 / portTICK_PERIOD_MS);
    if (err) ESP_LOGE(TAG, "tmp1075 write_reg: %s", esp_err_to_name(err));
    i2c_cmd_link_delete(cmd);
    return err;
}

// tmp1075_detect @ FUN_0x400e7df0 — up to 5 attempts: verify die-ID, set CFGR.
static bool tmp1075_detect(void)
{
    for (int attempt = 0; attempt < 5; attempt++) {
        ESP_LOGI(TAG, "Attempting to detect TMP1075");
        uint16_t id;
        esp_err_t err = tmp1075_read_reg(TMP1075_REG_DIEID, &id);
        if (err == ESP_OK) {
            if ((id >> 8) == 0x75) {
                err = tmp1075_write_reg(TMP1075_REG_CFGR, 0x0100);
                if (err == ESP_OK) { ESP_LOGI(TAG, "TMP1075 detected and configured"); return true; }
                ESP_LOGE(TAG, "Could not write to CFGR: %s", esp_err_to_name(err));
            } else {
                ESP_LOGE(TAG, "Wrong device ID, ID is 0x%x", id >> 8);
            }
        } else {
            ESP_LOGE(TAG, "Could not read device ID from TMP1075: %s", esp_err_to_name(err));
        }
        ESP_LOGW(TAG, "TMP1075 configure attempt %u failed", attempt + 1);
    }
    return false;
}

// measurement_read_temperature @ FUN_0x400e7eb4 — one-shot conversion, up to 5 tries.
bool measurement_read_temperature(float* out)
{
    for (int attempt = 0; attempt < 5; attempt++) {
        esp_err_t err = tmp1075_write_reg(TMP1075_REG_CFGR, 0x8100); // OS=1 (one-shot)
        if (err == ESP_OK) {
            vTaskDelay(2);                       // settle (FUN_0x40093438(2))
            uint16_t raw;
            err = tmp1075_read_reg(TMP1075_REG_TEMP, &raw);
            if (err == ESP_OK) {
                float t = (raw >> 4) * 0.0625f;  // 12-bit, 0.0625 C/LSB
                if ((int16_t)raw < 0) t -= 256.0f;
                *out = t;
                return true;
            }
            ESP_LOGE(TAG, "Could not read from TEMP: %s", esp_err_to_name(err));
        } else {
            ESP_LOGE(TAG, "Could not write to CFGR: %s", esp_err_to_name(err));
        }
        ESP_LOGW(TAG, "TMP1075 get reading attempt %u failed", attempt + 1);
    }
    return false;
}

// ---- task / init ----------------------------------------------------------

static void measurement_task(void* arg);   // FUN @ PTR_LAB_400d166c — the reading loop

// measurement_init @ measurement.cpp:0x140 (FUN_0x400e2254)
// On a cold boot (not a deep-sleep wake) resets accelerometer state; creates the
// start/finished queues; loads temperature_offset; inits accel + TMP1075; then
// spawns the measurement task.
void measurement_init(void)
{
    if (esp_reset_reason() != ESP_RST_DEEPSLEEP) {   // FUN_0x400d4d98 != 8
        accel_reset_state();                         // PTR_FUN_400d1674
        // zero running accumulators (tilt history etc.)
    }

    s_start_q = xQueueCreate(5, 4);                  // FUN_0x400914e8(5,4,0)
    assert(s_start_q != NULL /* "measurement_start != NULL" */);
    s_finished_q = xQueueCreate(5, 40);
    assert(s_finished_q != NULL /* "measurement_finished != NULL" */);

    // Apply the saved temperature offset to the reading pipeline.
    int32_t temp_offset = nonvolatile_get_u32(&NVS_temp_offset, NULL); // FUN_0x400eb1d8
    measurement_set_temp_offset(temp_offset);        // PTR_FUN_400d163c

    if (!accel_init()) {                             // FUN_0x400e046c
        ESP_LOGW(TAG, "Could not init accelerometer");
    } else if (!accel_present()) {                   // FUN_0x400e12b4
        /* no accelerometer fitted */
    } else {
        accel_configure();                           // FUN_0x400e05a4
        if (!tmp1075_detect())                        // FUN_0x400e7df0
            ESP_LOGW(TAG, "Could not init temperature sensor");
    }

    if (xTaskCreate(measurement_task, "measurement", 0x1400, NULL,
                    configMAX_PRIORITIES - 1, NULL) != pdPASS)    // FUN_0x40094158
        ESP_LOGE(TAG, "Could not create measurement task");
}

// measurement_wait @ FUN_0x400e23bc — block up to 1500 ms for a finished reading.
int measurement_wait(measurement_t* out)
{
    measurement_t rec;
    if (xQueueReceive(s_finished_q, &rec, 1500 / portTICK_PERIOD_MS) == pdTRUE) {
        if (rec.status == 0) *out = rec;             // copy 0x24 bytes on success
        return rec.status;
    }
    ESP_LOGE(TAG, "Timed out waiting for the reading");
    return ESP_ERR_TIMEOUT;                          // 0x107
}

// measurement_request @ FUN_0x400e2430 — hand a request to the task (warns if one is queued).
void measurement_request(int req)
{
    extern int g_pending_request;                    // *DAT_400d1628
    if (g_pending_request) ESP_LOGW(TAG, "Overwriting queued data");
    g_pending_request = req;
}

// ---- settings / NVS key registry ------------------------------------------

// settings_register_keys @ FUN_0x400e25a8
// Declares every persistent key as a NonVolatileKeyDef node (name + default)
// and links it into the type registries used by nonvolatile_erase(). Recovered
// keys and defaults (NVS key <=15 chars; see docs for the JSON field aliases):
//
//   registeredaws   u32  0          device registered with AWS backend
//   test_api        bool 0          use test.rapt.io endpoints
//   cal_state       u32  0          calibration state (none/1pt/2pt/custom)
//   dev_opt         bool 0          developer options enabled
//   log_int         u32  172800     log upload interval (s)  [0x2a300]
//   log_whenfull    bool 1          upload the log when the buffer fills
//   telem_method    u32  1          telemetry method (ble/wifi)
//   interval_b      u32  300        telemetry interval, BLE (s)
//   grav_v_time     u32  86400      gravity-velocity window (s) [0x15180]
//   low_batt        bool 0          low-battery shutdown enabled
//   smart_mode      bool 1          smart advertising mode
//   smart_period    u32  30         smart-mode period (s) [0x1e]
//   boot_unstable   ...             unstable-boot counter
//   cal_d1,cal_a1   float           calibration point 1 density/angle
//   cal_d2,cal_a2   float           calibration point 2 density/angle
//   adc_cal         float           battery ADC calibration
//   refresh_tok     str             cloud refresh token
//   temp_offset     s32  0          temperature offset
//   fw_reported     str             last firmware version reported to cloud
//   fw_failure      ...             firmware-update-failure flag
//   fw_rollback     ...             firmware-rollback flag
void settings_register_keys(void)
{
    // (In the binary this is straight-line code building the linked descriptor
    // table; reconstructed as a data table for readability.)
}

// ---- reading loop + telemetry builder -------------------------------------

// measurement_task @ FUN @ PTR_LAB_400d166c (body not individually decompiled):
// waits on measurement_start, performs an accel + temperature read, derives
// gravity (gravity_calc), fills a measurement_t and posts it to
// measurement_finished.
static void measurement_task(void* arg) { /* reading loop — see above */ }

// build_reading_report @ FUN_0x400db9d0 (size 1463) — Medium confidence.
// Assembles the per-reading telemetry object and evaluates fermentation
// automation alerts. Observed JSON keys: "timestamp", "data.point",
// "data.gravity", "data.calibration_state"; emits human text
// "Gravity %s is %s the target %s.%s%s" and automation events
// "Automation Started"/"Automation Stopped". Reads are classified
// ("Reading not counted: %s", "a gesture", "a calibration run").
// Full field-level reconstruction left as structural summary.

// =============================================================================
// Telemetry payload + settings (extension; recovered from the "measure"/"battery"
// tagged functions). Addresses are Ghidra FUN_<addr> symbols. Confidence: HIGH
// (JSON keys recovered verbatim).
// =============================================================================

// FUN_0x400db864 - telemetry_add_reading(doc): add the per-reading telemetry
// fields to an open JSON document. Keys (verbatim) and sources:
//   rssi                  = wifi RSSI                         (FUN_0x400e695c + DAT_0x400d0cbc)
//   running_profile(bool) = is an automation running          (FUN_0x400da118)
//   profile_id (string)   = automation id, if any
//   profile_step_progress = current step progress (>=0)
//   temperature           = latest temperature (degC)         (reading @ FUN_0x400e1330)
//   gravity               = latest specific gravity
//   gravity_velocity      = SG change rate (only if valid)
//   <x>, <y>, <z>         = raw tilt components (DAT_0x400d0ca8/cac/cb0)
//   tilt_angle / angle    = tilt angle (deg) - emitted under both keys
//   battery               = charge fraction * scale, if calibrated (FUN_0x400dcd9c)
// All numbers are added as doubles via js_add_number (FUN_0x400e94f8).
void telemetry_add_reading(js_value *doc)
{
    js_add_i64(doc, "ts", wall_clock_now());              // FUN_0x400e9540 (timestamp)
    network_rssi_refresh();                               // FUN_0x400e695c
    js_add_number(doc, "rssi", (double)network_rssi());   // DAT_0x400d0cbc

    char profile_id[0x40]; int step = -1;
    bool running = automation_running(profile_id, sizeof profile_id, &step); // FUN_0x400da118
    js_add_bool(doc, "running_profile", running);         // FUN_0x400e951c
    if (running) {
        if (profile_id[0]) js_add_string(doc, "profile_id", profile_id);
        if (step >= 0)     js_add_number(doc, "profile_step_progress", (double)step);
    }

    reading_t r;
    if (latest_reading(&r)) {                              // FUN_0x400e1330
        js_add_number(doc, "temperature",      r.temperature);     // +0x14
        js_add_number(doc, "gravity",          r.gravity);         // +0x18
        if (r.has_velocity)
            js_add_number(doc, "gravity_velocity", r.gravity_velocity); // +0x20
        js_add_number(doc, "x", r.ax);
        js_add_number(doc, "y", r.ay);
        js_add_number(doc, "z", r.az);
        js_add_number(doc, "tilt_angle", r.tilt_angle);
        js_add_number(doc, "angle",      r.tilt_angle);
        float charge = (float)battery_voltage_to_charge(r.battery_mv);  // FUN_0x400dcd9c
        if (charge >= 0.0f)
            js_add_number(doc, "battery", charge * BATTERY_PERCENT_SCALE); // DAT_0x400d0c20
    }
}

// FUN_0x400db7b0 - settings_add_reported(doc): add the cached telemetry/log/
// power settings to a reported-state document (keys: log_upload_interval,
// log_upload_when_full, telemetry_method, telemetry_interval_wifi/ble,
// gravity_velocity_time, temperature_offset, low_battery_shutdown, smart_mode,
// smart_mode_period). Uses the typed js_add_* helpers (FUN_0x400f1380/13a4/13c8).
void settings_add_reported(js_value *doc);

// FUN_0x400dad74 - build_reported_state(doc): the full device shadow "reported"
// block - ip_sta (or "0.0.0.0"), gravity_expr_valid, last_refusal, refresh_token
// presence, adc_calibration, running_profile, all intervals/methods, uptime,
// battery, smart-mode. (If the measurement worker already reconstructed this,
// this is the same function @ 0x400dad74; kept as the canonical reference.)
void build_reported_state(js_value *doc);

// FUN_0x400e61dc - gravity_velocity_update(): maintain the gravity-velocity
// estimate and the "reports_successful"/"gravity_velocity_age" counters; reads
// tilt_angle/temperature/battery from the latest reading and the refresh_token
// from NVS. (Bookkeeping summarised; key names verbatim.)
void gravity_velocity_update(void);

// FUN_0x400e281c - wall_clock_now()/rebase: 64-bit wall clock; logs "Wall clock
// stepped by %lld s, rebasing ..." when NTP/cloud time corrects the estimate.
int64_t wall_clock_now(void);

// FUN_0x400e24e0 - export_write_gravity_rows(sink): emit stored gravity history
// as "%u,%.9g\n" CSV rows (timestamp, gravity) to an export sink.
void export_write_gravity_rows(void *sink);

// FUN_0x400e72b0 - round helper using floorf()/floor() for gravity/number
// formatting (SDK libm "floor").
