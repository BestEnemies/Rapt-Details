// web_config.cpp — RAPT Pill 2.0.0, reverse-engineered (readable reconstruction)
//
// Confidence: High. One large switch keyed on the request URI. The original is
// a chain of strcmp()s against the path (req->uri just past a prefix); it is
// rendered here as a readable dispatch. Each branch's effect, the header/body
// it reads, and the JSON it returns are all taken verbatim from the strings.
//
// Settings are stored through the `nonvolatile` layer (nvs_set_* helpers).
// Numeric values come in "X-Custom-*" headers; booleans sometimes in the body.
#include "web_config.h"
#include "nonvolatile.h"
#include "measurement.h"
#include "gravity_calc.h"
#include "power_management.h"
#include "aws_shadow.h"    // settings_changed() -> publish desired/reported
#include "esp_log.h"
#include "esp_system.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "pill_ap";

// Shared response scratch buffers (DAT_400d19d0 = 0x200, DAT_400d199c = 0x140).
static char g_resp_big[0x200];
static char g_resp_small[0x140];

// ---- response helper --------------------------------------------------------
// send_json @ 0x400e3a80 — 200 OK, application/json, body=whole string.
static void send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
}

// Read one "X-Custom-*" header into buf; returns ESP_OK (0) on success.
// (Ghidra FUN_40123a88 — thin wrapper over httpd_req_get_hdr_value_str.)
static esp_err_t get_custom_header(httpd_req_t *req, const char *name, char *buf, size_t len);
// Read the request body (FUN_40123ff0 — httpd_req_recv). Returns bytes read.
static int recv_body(httpd_req_t *req, char *buf, size_t len);

// NVS keys referenced by the portal (names from the measurement/shadow scans).
// (Exact descriptor objects live in nonvolatile.cpp.)
//  dev_opt, beta, test_api, telemetry_method, telemetry_interval_wifi/ble,
//  gravity_velocity_time, smart_mode, low_battery_shutdown, temperature_offset.

// =============================================================================
// pill_config_post_handler @ 0x400e40cc
// NOTE on URI matching: the original compares `req->uri + 8` (skips the leading
// path prefix) against each literal with strcmp(); the first match wins.
esp_err_t pill_config_post_handler(httpd_req_t *req)
{
    const char *uri = req->uri + 8; // skip prefix /* inferred offset from decompile */
    char hdr[0x1c];
    char body[8];

    // ---- calibration: one- and two-point -----------------------------------
    if (strcmp(uri, "/cal_1p.json") == 0 || strcmp(uri, "/cal_2p.json") == 0) {
        double angle1, dens1, angle2, dens2;
        esp_err_t e;
        if ((e = get_custom_header(req, "X-Custom-angle1", hdr, sizeof hdr))) {
            ESP_LOGE(TAG, "Could not get angle1 from headers: %d", e);
            goto cal_status_false;
        }
        angle1 = strtod(hdr, nullptr);
        if ((e = get_custom_header(req, "X-Custom-dens1", hdr, sizeof hdr))) {
            ESP_LOGE(TAG, "Could not get density1 from headers: %d", e);
            goto cal_status_false;
        }
        dens1 = strtod(hdr, nullptr);

        bool ok;
        if (strcmp(uri, "/cal_1p.json") == 0) {
            ok = (calibration_set_one_point(angle1, dens1) == 0);  // FUN_400dfb7c
        } else {
            if ((e = get_custom_header(req, "X-Custom-angle2", hdr, sizeof hdr))) {
                ESP_LOGE(TAG, "Could not get angle2 from headers: %d", e);
                goto cal_status_false;
            }
            angle2 = strtod(hdr, nullptr);
            if ((e = get_custom_header(req, "X-Custom-dens2", hdr, sizeof hdr))) {
                ESP_LOGE(TAG, "Could not get density2 from headers: %d", e);
                goto cal_status_false;
            }
            dens2 = strtod(hdr, nullptr);
            ok = (calibration_set_two_point(angle1, dens1, angle2, dens2) == 0); // FUN_400dfd5c
        }
        if (ok) { calibration_state_changed(); settings_changed(); } // FUN_400e1b78 / FUN_400f1098
        snprintf(g_resp_big, sizeof g_resp_big, "{\"status\": \"%s\"}", ok ? "true" : "false");
        send_json(req, g_resp_big);
        return ESP_OK;
    cal_status_false:
        snprintf(g_resp_big, sizeof g_resp_big, "{\"status\": \"%s\"}", "false");
        send_json(req, g_resp_big);
        return ESP_OK;
    }

    // ---- calibration state / gravity expression -----------------------------
    if (strcmp(uri, "/set_calibration_state.json") == 0) {
        const char *valid = "false";
        if (gravity_expr_set_from_header(req, "X-Custom-expr") == 0) { // FUN_400dfa88
            calibration_state_changed();      // FUN_400e1b78(0,"false")
            settings_changed();               // FUN_400f1098
            valid = "true";
        }
        snprintf(g_resp_big, sizeof g_resp_big, "{\"expr\": \"%s\", \"expr-valid\": %s}",
                 gravity_expr_string(), valid); // FUN_400dfa80 returns the expr text
        send_json(req, g_resp_big);
        return ESP_OK;
    }

    // ---- one-shot actions ---------------------------------------------------
    if (strcmp(uri, "/ota_now.json") == 0) {
        ota_trigger_now();                    // FUN_400e36c8 (sets OTA request flag)
        send_json(req, "{\"success\": true}");
        return ESP_OK;
    }
    if (strcmp(uri, "/poke_ap_timeout.json") == 0) {
        power_poke_ap_timeout();              // FUN_400e4d68
        send_json(req, "{\"success\": true}");
        return ESP_OK;
    }
    if (strcmp(uri, "/factory_reset.json") == 0) {
        nvs_erase_all();                      // FUN_400eb104
        esp_restart();                        // FUN_400d4a58  (does not return)
    }
    if (strcmp(uri, "/reboot.json") == 0) {
        esp_restart();                        // FUN_400d4a58  (does not return)
    }

    // ---- developer options (body "true"/"false") ----------------------------
    if (strcmp(uri, "/set_dev_opts.json") == 0) {
        int n = recv_body(req, body, 7);
        if (n < 1) { ESP_LOGE(TAG, "Could not read body of request to /set_dev_opts.json: %d", n);
                     return ESP_OK; }
        body[n] = '\0';
        bool on = (strcmp(body, "true") == 0);
        nvs_set_u32(KEY_DEV_OPT, on);         // FUN_400eb290
        if (on) {
            // Raise the BLE telemetry interval floor to 900 s if currently lower.
            uint32_t ble_int = nvs_get_u32(KEY_INTERVAL_BLE, 0);
            if (/*set*/ ble_int < 900) { nvs_set_u32(KEY_INTERVAL_BLE, 900); telemetry_reload(); }
        }
        send_json(req, "{\"success\": true}");
        return ESP_OK;
    }

    // ---- beta channel (body) ------------------------------------------------
    if (strcmp(uri, "/set_beta.json") == 0) {
        int n = recv_body(req, body, 7);
        if (n < 1) { ESP_LOGE(TAG, "Could not read body of request to /set_beta.json: %d", n);
                     return ESP_OK; }
        body[n] = '\0';
        nvs_set_u32(KEY_BETA_UPDATES, strcmp(body, "true") == 0);
        send_json(req, "{\"success\": true}");
        return ESP_OK;
    }

    // ---- test API endpoint (body) -------------------------------------------
    if (strcmp(uri, "/set_test_api.json") == 0) {
        int n = recv_body(req, body, 7);
        if (n < 1) { ESP_LOGE(TAG, "Could not read body of request to /set_test_api.json: %d", n);
                     return ESP_OK; }
        body[n] = '\0';
        nvs_set_u32(KEY_TEST_API, strcmp(body, "true") == 0);
        reconfigure_endpoints();              // FUN_400e6a38
        aws_iot_credentials_changed();        // FUN_400f11e4
        nvs_erase_key_cached(KEY_CERT_PEM);   // FUN_400eb8ec(*DAT_400d0c18)
        send_json(req, "{\"success\": true}");
        return ESP_OK;
    }

    // ---- telemetry interval (wifi/ble), header X-Custom-interval ------------
    if (strcmp(uri, "/set_interval_wifi.json") == 0 || strcmp(uri, "/set_interval_ble.json") == 0) {
        char ibuf[0x10];
        if (get_custom_header(req, "X-Custom-interval", ibuf, sizeof ibuf) == 0) {
            long v = atol(ibuf);
            bool is_ble = (strcmp(uri, "/set_interval_ble.json") == 0);
            bool dev = nvs_get_u32(KEY_DEV_OPT, 0);
            long clamped;
            if (!is_ble) {
                clamped = (v < 20) ? 20 : v;                 // wifi: min 20 s
                nvs_set_u32(KEY_INTERVAL_WIFI, clamped);
            } else {
                long floor = dev ? 60 : 900;                 // ble: 60 s if dev, else 900 s
                clamped = (v < floor) ? floor : (v > 0x2a30 ? 0x2a30 : v); // max 10800 s
                nvs_set_u32(KEY_INTERVAL_BLE, clamped);
            }
            telemetry_reload();               // FUN_400e1c04
            settings_changed();               // FUN_400f1098
        } else {
            ESP_LOGE(TAG, "Could not get interval ...");
        }
        snprintf(g_resp_small, sizeof g_resp_small, "{\"interval\": %u}",
                 (unsigned)telemetry_interval_current()); // FUN_400e1310
        send_json(req, g_resp_small);
        return ESP_OK;
    }

    // ---- clear calibration --------------------------------------------------
    if (strcmp(uri, "/clear_calibration.json") == 0) {
        calibration_clear();                  // FUN_400dfa00
        send_json(req, "{\"success\": true}");
        return ESP_OK;
    }

    // ---- upload log ---------------------------------------------------------
    if (strcmp(uri, "/upload_log.json") == 0) {
        log_upload_now();                     // FUN_400e1278
        send_json(req, "{\"success\": true}");
        return ESP_OK;
    }

    // ---- report telemetry now ----------------------------------------------
    if (strcmp(uri, "/telem_now.json") == 0) {
        bool ok = network_is_connected();     // FUN_400e6a08
        if (ok) report_now();                 // FUN_400e1360
        send_json(req, ok ? "{\"success\": true}" : "{\"success\": false}");
        return ESP_OK;
    }

    // ---- telemetry method (header X-Custom-telem_method = "bluetooth"/"wifi") -
    if (strcmp(uri, "/set_telem_method.json") == 0) {
        char m[0x10];
        if (get_custom_header(req, "X-Custom-telem_method", m, sizeof m) != 0) {
            ESP_LOGE(TAG, "Could not get telemetry ...");
            send_json(req, "{\"success\": false}");
            return ESP_OK;
        }
        bool is_ble = (strcmp(m, "bluetooth") == 0);
        if (is_ble) ble_telemetry_selected();            // FUN_400e1bc8(0,0)
        nvs_set_u32(KEY_TELEMETRY_METHOD, is_ble);       // bluetooth=1 /* inferred */
        reconfigure_endpoints();                         // FUN_400e6a38
        telemetry_reload();                              // FUN_400e1c04
        send_json(req, "{\"success\": true}");
        return ESP_OK;
    }

    // ---- temperature offset (header, float) ---------------------------------
    if (strcmp(uri, "/set_temp_offset.json") == 0) {
        if (get_custom_header(req, "X-Custom-temp_offset", hdr, sizeof hdr) == 0) {
            double off = strtod(hdr, nullptr);
            nvs_set_float(KEY_TEMP_OFFSET, off);         // FUN_400eb418
            settings_changed();                          // FUN_400f1098
            snprintf(g_resp_small, sizeof g_resp_small, "{\"temp_offset\": %g}", off);
            send_json(req, g_resp_small);
        } else {
            ESP_LOGE(TAG, "Could not get temp offset from headers: ...");
            send_json(req, "{\"temp_offset\": 0.0}");
        }
        return ESP_OK;
    }

    // ---- gravity-velocity time window (header, u32) -------------------------
    if (strcmp(uri, "/set_grav_vel_time.json") == 0) {
        char v[0x10];
        if (get_custom_header(req, "X-Custom-grav_vel_time", v, sizeof v) != 0) {
            ESP_LOGE(TAG, "Could not get gravity velocity time limit from headers: ...");
            send_json(req, "{\"grav_vel_time\": 0}");
            return ESP_OK;
        }
        uint32_t t = atol(v);
        nvs_set_u32(KEY_GRAV_VEL_TIME, t);
        gravity_velocity_reconfigure();                  // FUN_400e2560
        settings_changed();
        snprintf(g_resp_small, sizeof g_resp_small, "{\"grav_vel_time\": %u}", (unsigned)t);
        send_json(req, g_resp_small);
        return ESP_OK;
    }

    // ---- smart mode (header, bool) ------------------------------------------
    if (strcmp(uri, "/set_smart_mode.json") == 0) {
        char v[0x10];
        if (get_custom_header(req, "X-Custom-smart_mode", v, sizeof v) != 0) {
            ESP_LOGE(TAG, "Could not get smart_mode setting from headers: ...");
            send_json(req, "{\"smart_mode\": 0}");
            return ESP_OK;
        }
        uint32_t on = (atol(v) != 0);
        nvs_set_u32(KEY_SMART_MODE, on);
        reconfigure_endpoints();  settings_changed();
        snprintf(g_resp_small, sizeof g_resp_small, "{\"smart_mode\": %u}", (unsigned)on);
        send_json(req, g_resp_small);
        return ESP_OK;
    }

    // ---- low-battery shutdown (header, bool) --------------------------------
    if (strcmp(uri, "/set_low_battery_shutdown.json") == 0) {
        char v[0x10];
        if (get_custom_header(req, "X-Custom-low_batt_shutdown", v, sizeof v) != 0) {
            ESP_LOGE(TAG, "Could not get low battery shutdown setting from headers: ...");
            send_json(req, "{\"low_batt_shutdown\": 0}");
            return ESP_OK;
        }
        uint32_t on = (atol(v) != 0);
        nvs_set_u32(KEY_LOW_BATT_SHUTDOWN, on);
        settings_changed();
        snprintf(g_resp_small, sizeof g_resp_small, "{\"low_batt_shutdown\": %u}", (unsigned)on);
        send_json(req, g_resp_small);
        return ESP_OK;
    }

    // Unrecognised path → 404.
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, nullptr); // FUN_40123e58(req,6,0)
    return ESP_OK;
}
