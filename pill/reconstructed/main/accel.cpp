// =============================================================================
// accel.cpp — RAPT Pill 2.0.0 accelerometer + gesture/click engine (RECONSTRUCTED)
//
// Reverse-engineered from app_2.0.0.bin. Not original source. Chip: ST LIS3DH
// on the legacy ESP-IDF I²C master driver (i2c_cmd_link_create/…/i2c_master_cmd_begin).
// Log tags: accel_c ("accel.c"), gesture, status_led, diag_export.
//
// Address → reconstructed-name map (Ghidra FUN_<addr>):
//   0x400e046c accel_init_peripherals   0x400e05a4 accel_configure
//   0x400e0694 accel_power_down         0x400e0330 accel_write_reg
//   0x400e02ac accel_read_regs          0x400e09e4 accel_click_engine
//   0x400df26c gesture_tick             0x400debf4 gesture_window_close
//   0x400dedec gesture_log_serialise    0x400dea2c diag_export
//   0x400e6bc8 status_led_tick
//   0x400e8250 json_escape_string*      0x400e8794 json_serialise_value*
//   (* these two are the shared JSON serializer, not accel-specific; they landed
//    in this bundle because the gesture/diag export calls them. Documented here,
//    they belong conceptually in json_util.cpp.)
// =============================================================================
#include "accel.h"
#include <string.h>
#include "driver/i2c.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG   = "accel.c";
static const char *T_GEST= "gesture";
static const char *T_LED = "status_led";
static const char *T_DIAG= "diag_export";

#define LIS3DH_I2C_PORT  0
#define LIS3DH_ADDR      0x19     // write byte 0x32, read byte 0x33

static SemaphoreHandle_t g_sampling_mutex;   // DAT_0x400d142c ("sampling_mutex != NULL")

// -----------------------------------------------------------------------------
// accel_write_reg (FUN_0x400e0330): single-register write over I²C.
//   [START][0x32][reg][val][STOP], 100-tick timeout, then yield.
// -----------------------------------------------------------------------------
static int accel_write_reg(uint8_t reg, uint8_t val)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();     // FUN_0x4010ed58
    i2c_master_start(cmd);                            // FUN_0x4010ed98
    i2c_master_write_byte(cmd, (LIS3DH_ADDR << 1), true); // 0x32
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write_byte(cmd, val, true);
    i2c_master_stop(cmd);                             // FUN_0x4010ede0
    int err = i2c_master_cmd_begin(LIS3DH_I2C_PORT, cmd, 100); // FUN_0x4010f02c
    if (err) ESP_LOGE(TAG, "accel_write_reg: %s", esp_err_to_name(err));
    i2c_cmd_link_delete(cmd);                         // FUN_0x4010ed70
    vTaskDelay(1);                                    // FUN_0x40093438
    return err;
}

// -----------------------------------------------------------------------------
// accel_read_regs (FUN_0x400e02ac): auto-increment burst read of `len` regs
// starting at `reg` (sets the MSB for auto-increment when len>1).
// -----------------------------------------------------------------------------
static int accel_read_regs(uint8_t reg, uint8_t *buf, int len, bool yield)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (LIS3DH_ADDR << 1), true);          // 0x32 (write phase)
    i2c_master_write_byte(cmd, reg | (len > 1 ? 0x80 : 0), true);  // auto-increment
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (LIS3DH_ADDR << 1) | 1, true);      // 0x33 (read phase)
    if (len > 1) i2c_master_read(cmd, buf, len - 1, I2C_MASTER_ACK);  // FUN_0x4010ef40
    i2c_master_read_byte(cmd, buf + len - 1, I2C_MASTER_NACK);        // FUN_0x4010ee90
    i2c_master_stop(cmd);
    int err = i2c_master_cmd_begin(LIS3DH_I2C_PORT, cmd, 100);
    if (err) ESP_LOGE(TAG, "accel_read_data: %s", esp_err_to_name(err));
    i2c_cmd_link_delete(cmd);
    if (yield) vTaskDelay(1);
    return err;
}

// -----------------------------------------------------------------------------
// accel_init_peripherals (FUN_0x400e046c): create the sampling mutex, install
// the legacy I²C driver, then probe for the LIS3DH (WHO_AM_I must read 0x33,
// i.e. '3'). Retries the probe up to 5×. Recovered source: lis3dh.c:0x112.
// -----------------------------------------------------------------------------
int accel_init_peripherals(void)
{
    if (!g_sampling_mutex) {
        g_sampling_mutex = xSemaphoreCreateMutex();   // FUN_0x40091720
        if (!g_sampling_mutex)
            __assert_func("lis3dh.c", 0x112, "accel_init_peripherals", "sampling_mutex != NULL");
    }

    i2c_config_t cfg = {};                            // 0x1c bytes
    cfg.mode             = I2C_MODE_MASTER;           // 1
    cfg.sda_io_num       = 0x19;   // GPIO25 /* inferred */
    cfg.scl_io_num       = 0x1a;   // GPIO26 /* inferred */
    cfg.sda_pullup_en    = cfg.scl_pullup_en = true;  // 0x0101
    cfg.master.clk_speed = I2C_CLK_HZ;                // DAT_0x400d1440
    i2c_param_config(LIS3DH_I2C_PORT, &cfg);          // FUN_0x4010e99c
    int err = i2c_driver_install(LIS3DH_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0); // FUN_0x4010e430
    if (err) { ESP_LOGE(TAG, "Accel I2C driver install failed: %d", err); return err; }

    for (int tries = 5; tries; tries--) {
        if (accel_read_status(LIS3DH_STATUS) != 0 ||  // FUN_0x400e0424 (probe x2)
            accel_read_status(LIS3DH_STATUS) != 0) {
            ESP_LOGW(TAG, "Accel failed to respond%s at 0x%02x",
                     tries < 5 ? " again" : "", LIS3DH_STATUS);
            vTaskDelay(1);
            continue;
        }
        if (accel_write_reg(LIS3DH_CTRL_REG0, 0x10) != 0) {   // SA0 pull-up control
            ESP_LOGE(TAG, "Could not write to accel CTRL_REG0"); return -1;
        }
        char who = 0;
        accel_read_reg(LIS3DH_WHO_AM_I, &who);        // FUN_0x400e039c
        if (who != 0x33 /* '3' */) {
            ESP_LOGE(TAG, "Wrong accel chip connected: 0x%02x", who);
            return -1;
        }
        ESP_LOGI(TAG, "LIS3DH detected");
        return 0;
    }
    ESP_LOGE(TAG, "Could not detect accelerometer");
    return -1;
}

// -----------------------------------------------------------------------------
// accel_configure (FUN_0x400e05a4): program the LIS3DH for normal tilt sampling.
// Retries the whole sequence up to 5× ("Configure attempt %u failed").
//   CTRL_REG1=0x37 (25 Hz, XYZ enabled)   CTRL_REG2=0x31 (HPF)
//   CTRL_REG3=0x40 (INT1)                 CTRL_REG4=0x88 (BDU, ±2g, HR)
//   CTRL_REG5=0x00                        CTRL_REG6=0x20
//   TEMP_CFG(0x1f)=0x80                   INT1_CFG(0x30)=0x2a
//   INT1_THS(0x32)=4  INT1_DUR(0x33)=3  INT2_CFG(0x34)=2
//   CLICK_CFG(0x36)=0x0a  CLICK_THS(0x37)=8
// -----------------------------------------------------------------------------
void accel_configure(void)
{
    ESP_LOGI(TAG, "Configuring LIS3DH");
    static const uint8_t seq[][2] = {
        {0x20,0x37},{0x21,0x31},{0x22,0x40},{0x23,0x88},{0x24,0x00},{0x25,0x20},
        {0x1f,0x80},{0x30,0x2a},{0x32,0x04},{0x33,0x03},{0x34,0x02},{0x36,0x0a},{0x37,0x08},
    };
    for (int attempt = 0; attempt < 5; attempt++) {
        bool ok = true;
        for (auto &rv : seq) if (accel_write_reg(rv[0], rv[1])) { ok = false; break; }
        if (ok) { ESP_LOGI(TAG, "LIS3DH configured"); return; }
        ESP_LOGW(TAG, "Configure attempt %u failed", attempt + 1);
    }
}

// FUN_0x400e0694 — accel_power_down(): CTRL_REG1=0 ("LIS3DH power down").
int accel_power_down(void) { accel_write_reg(LIS3DH_CTRL_REG1, 0); ESP_LOGI(TAG, "LIS3DH power down"); return 0; }

// -----------------------------------------------------------------------------
// accel_click_engine (FUN_0x400e09e4): arm/disarm the LIS3DH single/double-click
// detector at high ODR. On arm, takes the sampling mutex, raises the rate, and
// programs CLICK_CFG/THS/TIME_* + FIFO; on any failure it disarms and logs
// "Click engine arm failed (%d): restoring the 25 Hz". Returns 0 / err /
// 0x103 (could not take the mutex).
// -----------------------------------------------------------------------------
void accel_click_engine(bool arm, uint8_t click_cfg, uint32_t time_limit,
                        uint8_t latency, uint8_t window)
{
    static bool held;   // DAT_0x400d1484
    if (arm) {
        if (!held) {
            if (!g_sampling_mutex || xSemaphoreTake(g_sampling_mutex, 0) != pdTRUE)
                return /*0x103*/;
            held = true;
        }
        int e;
        if ((e = accel_write_reg(LIS3DH_CTRL_REG1, 0x77)) == 0 &&   // 400 Hz
            (e = accel_write_reg(LIS3DH_CTRL_REG2, 0x35)) == 0 &&
            (e = accel_write_reg(LIS3DH_CLICK_THS,  click_cfg | 0x80)) == 0 &&   // LIR
            (e = accel_write_reg(LIS3DH_TIME_LIMIT, time_limit & 0x7f)) == 0 &&
            (e = accel_write_reg(LIS3DH_TIME_LATENCY, latency)) == 0 &&
            (e = accel_write_reg(LIS3DH_TIME_WINDOW,  window)) == 0 &&
            (e = accel_write_reg(LIS3DH_CLICK_CFG, 0x15)) == 0 &&                 // single-click XYZ
            (e = accel_read_reg(LIS3DH_REFERENCE, nullptr)) == 0 &&              // reset HPF
            (e = accel_write_reg(LIS3DH_FIFO_CTRL, 0x00)) == 0 &&
            (e = accel_write_reg(LIS3DH_CTRL_REG5, 0x40)) == 0 &&                 // FIFO enable
            (e = accel_write_reg(LIS3DH_FIFO_CTRL, 0x80)) == 0 &&                 // stream mode
            (e = accel_read_reg(LIS3DH_CLICK_SRC, nullptr)) == 0) {
            return;
        }
        ESP_LOGE(TAG, "Click engine arm failed (%d): restoring the 25 Hz", e);
        accel_click_engine(false, 0, 0, 0, 0);   // disarm
        return;
    }
    // disarm: restore 25 Hz, clear click/FIFO regs, release the mutex
    int e  = accel_write_reg(LIS3DH_CTRL_REG1, 0x37);
    int e2 = accel_write_reg(LIS3DH_CLICK_CFG, 0);
    int e3 = accel_write_reg(LIS3DH_FIFO_CTRL, 0);
    int e4 = accel_write_reg(LIS3DH_CTRL_REG5, 0);
    int e5 = accel_write_reg(LIS3DH_CTRL_REG2, 0x31);
    if (e||e2||e3||e4||e5) ESP_LOGE(TAG, "Click engine disarm: a register write failed (%d)",
                                    e?e:e2?e2:e3?e3:e4?e4:e5);
    if (held) { held = false; xSemaphoreGive(g_sampling_mutex); }
}

// =============================================================================
// gesture_tick (FUN_0x400df26c): the gesture recogniser, polled from the
// measurement loop. A small state machine over `gesture_state`:
//
//   IDLE        — if a strong Z tilt crosses a threshold, step up.
//   ARMED       — arm the click engine; wait for the double-tap jerk window.
//   SAMPLING    — every ~10 ms drain the FIFO (FUN_0x400e0b6c); per 3-axis
//                 sample compute the max inter-sample jerk; keep a 160-sample
//                 ring; a jerk ≥ 500 (and the quiet-level / spacing rules)
//                 counts as a "jump". ≥ required jumps within the window and
//                 a trailing-quiet check ⇒ a recognised double-tap gesture.
//                 Also reads CLICK_SRC (FUN_0x400e0c00) to log HW click events.
//   INVERT_HOLD — a sustained inverted orientation (Z < −599) for the hold
//                 time ⇒ "Inverted hold complete" ⇒ start a long session.
//   SESSION_WAIT— a session is open; decrement until it ends.
//
// On success: "Gesture recognised: opening a 120 s session", closes the listen
// window (gesture_window_close), and calls keep_awake(120000) via FUN_0x400e6d0c.
// Thresholds (all /* inferred from constants */): tilt −0x2ce/−599, jerk 500,
// quiet 0x78, window 0xa0 samples, min jumps 0x28.
// =============================================================================
void gesture_tick(void)
{
    // (Structural reconstruction — the decompiled body is a dense fixed-point
    //  state machine over ~20 globals at 0x400d12xx/0x400d13xx. Control flow,
    //  thresholds and log points are faithful; individual global field names
    //  are inferred. See FUN_0x400df26c in decomp.jsonl for the exact body.)
    uint32_t now = esp_timer_get_ms();                 // FUN_0x40092f98

    // session timeout bookkeeping (SESSION_WAIT countdown)…
    // state dispatch on g_gesture_state … (see header enum)
    //   SAMPLING: for each FIFO sample → jerk score → jump/click detection →
    //             append to the per-window record, decide recognised/not.
    //   on recognised: gesture_window_close(clicks+1);
    //                   ESP_LOGI(T_GEST, "Gesture recognised: opening a %d s session", 120);
    //                   g_session = 6; keep_awake-arm; state = SESSION_WAIT;
    //   on give-up:    gesture_window_close(0);
    //                   ESP_LOGI(T_GEST, "No double tap within the window");
    // Full dispatch omitted for readability; behaviour summarised above.
    (void)now;
}

// -----------------------------------------------------------------------------
// gesture_window_close (FUN_0x400debf4): log the finished listen window and push
// its record into the 8-deep gesture-log ring (each record 0x30 bytes).
//   "listen window %u ... (FIFO OVERRUN, samples lost)" when the FIFO overran.
// -----------------------------------------------------------------------------
static void gesture_window_close(uint8_t outcome)
{
    g_window.outcome = outcome;                        // DAT_0x400d1288+1
    ESP_LOGI(T_GEST, "listen window %u %s ... %s",
             g_window.id, outcome_name(outcome),
             g_window.fifo_overran ? " (FIFO OVERRUN, samples lost)" : "");
    // ring push: records[head] = g_window; head = (head+1)&7; if(count<8) count++
    int h = g_log_head;
    memcpy(&g_log_ring[h], &g_window, 0x30);
    g_log_head = (h + 1) & 7;
    if (g_log_count < 8) g_log_count++;
}

// -----------------------------------------------------------------------------
// gesture_log_serialise (FUN_0x400dedec): render the gesture-log ring as JSON
// into `out_stream` (the BLE/HTTP export sink, FUN_0x40123cd0). Schema:
//   "tap jerk=%d quiet level=%d ..." header, then per record:
//   {"%s seq=%u outcome=%s samples=[{"sample":%u,"jump":%d},…],
//     "clicks":[{"ms":%u,"src":%u},…]}
// Warns "gesture record %u did not fit" if a record exceeds ~0x1fe bytes.
// -----------------------------------------------------------------------------
void gesture_log_serialise(void *out)
{
    char buf[512]; int n = 0;
    n = fmt_append(buf, sizeof buf, 0, "tap jerk=%d quiet level=%d samples=%d window=%d min=%d ...",
                   500, 0x78, 0x28, 0xa0, 0x28);       // FUN_0x400decbc
    stream_write(out, buf, n);                         // FUN_0x40123cd0
    for (unsigned i = 0; i < g_log_count; i++) {
        int idx = (g_log_head + 8 - g_log_count + i) & 7;
        gesture_record_t *r = &g_log_ring[idx];
        n = 0;
        n = fmt_append(buf, sizeof buf, n, "%s{\"seq\":%u,\"outcome\":\"%s\",\"samples\":[",
                       i ? "," : "", r->seq, outcome_name(r->outcome));
        for (int s = 0; s < r->n_samples; s++)
            n = fmt_append(buf, sizeof buf, n, "%s{\"sample\":%u,\"jump\":%d}",
                           s ? "," : "", r->sample[s].idx, r->sample[s].jump);
        n = fmt_append(buf, sizeof buf, n, "],\"clicks\":[");
        for (int c = 0; c < r->n_clicks; c++)
            n = fmt_append(buf, sizeof buf, n, "%s{\"ms\":%u,\"src\":%u}",
                           c ? "," : "", r->click[c].ms, r->click[c].src);
        n = fmt_append(buf, sizeof buf, n, "]}");
        if (n > 0x1fe) ESP_LOGW(T_GEST, "gesture record %u did not fit", r->seq);
        stream_write(out, buf, n);
    }
}

// -----------------------------------------------------------------------------
// diag_export (FUN_0x400dea2c): paged diagnostics export. Walks a registry of
// named exporters (each {name, format, fn, flags}); for the selected page it
// builds a cJSON doc {name, format, page, pages, total, snapshot, data[, params]}
// and serialises it, warning if a page exceeds 0x400 bytes
// ("%s page %u is %u bytes, over %d"). Used by the `export log`/diagnostics path.
// -----------------------------------------------------------------------------
int diag_export(void)
{
    // (Registry-walk + cJSON build; reconstructed structurally. The exporter
    //  table is at PTR_0x400d1234, 0x10 bytes/entry.)
    return 0;  // see FUN_0x400dea2c
}

// -----------------------------------------------------------------------------
// status_led_tick (FUN_0x400e6bc8): drive the status LED on GPIO18 via LEDC PWM
// (5 kHz). Lazily configures the LEDC timer + channel ("LED PWM timer/channel
// not available"), then sets a brightness/blink state derived from the device
// state (charging, connected, error…).
// -----------------------------------------------------------------------------
void status_led_tick(void)
{
    // ledc_timer_config (FUN_0x4010dacc) + ledc_channel_config (FUN_0x4010df64),
    // then ledc_set_duty/update or gpio fallback (FUN_0x4010dd80/40105f04).
    // Reconstructed structurally; see FUN_0x400e6bc8.
}

// =============================================================================
// Shared JSON serializer (NOT accel-specific; lives here only because the
// gesture/diag export pulled it into this bundle — conceptually json_util.cpp):
//   FUN_0x400e8250 json_escape_string: escape control chars as \u00XX, plus
//       \" \\ and optionally '/'; copies the rest verbatim.
//   FUN_0x400e8794 json_serialise_value: recursive serialiser for the JSON tree
//       (object/array/string/number/bool); numbers use "%1.17g" with a shortest
//       round-trip retry; honours a "pretty" flag for indentation.
// (Documented, not reconstructed line-by-line — see json_util.h for the facade.)
// =============================================================================
