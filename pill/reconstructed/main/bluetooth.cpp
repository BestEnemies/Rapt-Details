// =============================================================================
// bluetooth.cpp — RAPT Pill 2.0.0 BLE command channel (RECONSTRUCTED)
//
// Reverse-engineered from app_2.0.0.bin (ESP-IDF 5.1 NimBLE host). Not original
// source. Confidence: HIGH for the command/auth protocol (corroborated by the
// live device in ble-protocol.md), MEDIUM for GAP/advertising glue.
//
// Log tags: ble_devinfo, ble_reg, ble_gap, ble_port, ble_mux, ble_wifi.
// The three custom GATT services and their access callbacks funnel every write
// through a single sub-characteristic router. Owner-auth gates commands and the
// device shadow; see owner-auth below (and RECONSTRUCTION_NOTES.md).
// =============================================================================
#include "bluetooth.h"
#include <string.h>
#include "esp_log.h"
#include "host/ble_gatt.h"   // NimBLE

// JSON helpers (a small JSON lib compiled into the app; cJSON-like).
extern void       *json_parse(const char *text);                 // FUN_0x400e91f8
extern int         json_type(const void *v);                     // PTR_FUN_400d08c4 (4 == object)
extern void       *json_object(const void *v);                   // PTR_FUN_400d096c
extern const char *json_get_string(const void *o, const char *k);// FUN_0x400e871c
extern const void *json_get(const void *o, const char *k);       // FUN_0x400e8670 / FUN_0x400e862c
extern void        json_free(void *v);                           // FUN_0x400e8a70

// cross-module effects invoked by commands
extern void keep_awake(uint32_t ms);                             // FUN_0x400e4de0
extern void report_now(void);                                    // FUN_0x400e1360
extern bool device_is_registered(void);                          // FUN_0x400e6a08
extern void upload_log(void);                                    // FUN_0x400e1278
extern void automation_start(const char *name);                 // FUN_0x400d9b84
extern void automation_stop(void);                               // FUN_0x400d9bb4
extern int  calibration_point(bool is_wort, double gravity, const char **err); // FUN_0x400ddbe0
extern bool calibration_cancel(void);                            // FUN_0x400ddc60
extern void calibration_clear(void);                             // FUN_0x400ddcd8
extern bool command_guard_busy(const char *cmd);                 // FUN_0x400e5b54 (refuse during update/cal)
extern void wifi_forget(void);                                   // FUN_0x400e983c
extern void wifi_restart_sta(void);                              // FUN_0x400ea0d4
extern void registration_clear(void);                           // FUN_0x400e128c
extern void factory_reset(void);                                 // FUN_0x400e12a0
extern void beta_updates_check_now(void);                        // FUN_0x400e5d6c
extern int  set_bool_setting(const char *cmd, const char *key, const void *args); // FUN_0x400e5a84
extern int  export_data(const char *what, uint32_t page);        // FUN_0x400de9a8
extern void own_mac_string(char *out12);                         // FUN_0x400e69c4
extern int  shadow_apply_desired(const void *desired);           // FUN_0x400ede24

static const char *T_DEVINFO = "ble_devinfo";
static const char *T_PORT    = "ble_port";

// =============================================================================
// Owner authentication  (authentication.cpp logic; tag ble_gap)
//
// A runtime flag + a 64-byte secret, both in RAM, both zeroed at every boot and
// with NO NVS copy. So owner-auth starts DISABLED after every reboot and is only
// turned on when a session provisions a secret (via the RAPT Brew app / cloud
// handshake). The secret is not recoverable from the firmware or NVS dump.
// =============================================================================
static owner_auth_state g_auth;   // flag @0x400d2468, secret @0x400d2484, …

// FUN_0x400ec720 — owner_is_authenticated(conn):
//   if auth is DISABLED, always true; otherwise the per-connection "authed" bit.
bool owner_is_authenticated(uint16_t conn)
{
    if (!g_auth.enabled) return true;
    ble_conn *c = ble_conn_find(conn);   // FUN_0x400ec5f0
    return c && c->authed;               // *(c+4)
}

// FUN_0x400ec740 — owner_authenticate(conn, buf, len): compare the written bytes
// to the stored secret (trailing whitespace trimmed). On match: mark this
// connection authed ("Unlocked by the peer"). 5 wrong tries → ~3 s lockout
// ("Wrong secret from the peer", "%d wrong secrets, owner authentication locked").
// (Full body in the notes; summarised here.)
static int owner_authenticate(uint16_t conn, const char *buf, uint32_t len);

// FUN_0x400ed634 — owner_set_secret(s): non-empty → strcpy + enable; empty/NULL
// → zero the secret + disable.
static void owner_set_secret(const char *s)
{
    if (!s || !*s) { memset(g_auth.secret, 0, sizeof g_auth.secret); g_auth.enabled = false; }
    else           { strcpy(g_auth.secret, s);                       g_auth.enabled = true;  }
}

// =============================================================================
// Sub-characteristic WRITE router   (FUN_0x400ee060)
//
// Every writable control characteristic funnels here with the sub-char id.
//   0x0C auth   : owner_authenticate()
//   0x0E command: require auth → parse JSON object → read "cmd"/"args" → dispatch
//   0x0B desired: require auth → parse JSON object → read "desired" → apply shadow
// Returns a BLE ATT error code.
// =============================================================================
static int ble_write_router(uint8_t sub, const char *buf, uint32_t len)
{
    switch (sub) {
    case SUB_AUTH: {                                   // 0x0C
        int rc = owner_authenticate(g_conn_handle, buf, len);
        if (rc) return rc;
        ESP_LOGI(T_DEVINFO, "Owner authenticated");
        return ATT_OK;
    }

    case SUB_COMMAND: {                                // 0x0E
        if (!owner_is_authenticated(g_conn_handle)) {
            ESP_LOGW(T_DEVINFO, "Command before owner authentication");
            return ATT_INSUFFICIENT_AUTH;
        }
        void *doc = json_parse(buf);
        if (!doc || json_type(doc) != 4 /*object*/) {
            ESP_LOGW(T_DEVINFO, "Command write (%u bytes) is not a JSON object", len);
            command_refused("command write is not a JSON object");   // FUN_0x400dad3c
            if (doc) json_free(doc);
            return ATT_WRITE_REQ_REJECTED;
        }
        const void *obj = json_object(doc);
        const char *cmd = json_get_string(obj, "cmd");
        if (!cmd) {
            ESP_LOGW(T_DEVINFO, "Command write has no \"cmd\" string: rejected");
            command_refused("command write has no \"cmd\" string");
            json_free(doc);
            return ATT_WRITE_REQ_REJECTED;
        }
        const void *args = json_get(obj, "args");
        int ok = command_dispatch(cmd, args);          // FUN_0x400e63e8
        if (!ok)
            ESP_LOGW(T_DEVINFO, "Direct command %s (%u bytes) refused", cmd, len);
        json_free(doc);
        return ok ? ATT_OK : ATT_CMD_FAILED;           // 0 or 0x0E
    }

    case SUB_DESIRED: {                                // 0x0B
        if (!owner_is_authenticated(g_conn_handle)) {
            ESP_LOGW(T_DEVINFO, "Desired write before owner authentication");
            return ATT_INSUFFICIENT_AUTH;
        }
        void *doc = json_parse(buf);
        if (!doc || json_type(doc) != 4) {
            ESP_LOGW(T_DEVINFO, "Desired write (%u bytes) is not a JSON object", len);
            command_refused("desired write is not a JSON object");
            if (doc) json_free(doc);
            return ATT_WRITE_REQ_REJECTED;
        }
        const void *desired = json_get(json_object(doc), "desired");
        if (!desired) {
            ESP_LOGW(T_DEVINFO, "Desired write has no \"desired\" object");
            command_refused("desired write has no \"desired\" object");
            json_free(doc);
            return ATT_WRITE_REQ_REJECTED;
        }
        int rc = shadow_apply_desired(desired);        // FUN_0x400ede24
        json_free(doc);
        return rc;
    }

    default:
        ESP_LOGW(T_DEVINFO, "Unknown sub-characteristic 0x%02x write (%u bytes)", sub, len);
        return ATT_READ_NOT_PERMITTED;                 // 6
    }
}

// =============================================================================
// Sub-characteristic READ router   (FUN_0x400ee314)
//
//   0x01 info     : device-info JSON (always readable)
//   0x0B/0x0D     : shadow desired / reported  (owner-auth required)
//   0x0F export   : export buffer               (owner-auth required)
// Builds a JSON document, serialises it, hands it to the GATT read reply.
// =============================================================================
static int ble_read_router(uint8_t sub)
{
    void *doc = nullptr;

    if (sub == SUB_INFO) {                             // 0x01 — device info
        doc = json_new_object();                       // FUN_0x400e8bcc
        if (!doc) { ESP_LOGE(T_DEVINFO, "Could not allocate JSON"); return ATT_INSUFFICIENT_RES; }
        char mac[18]; mac_address_string(mac, 0x12);   // FUN_0x400ec210
        json_add_string(doc, "device_type",     device_type_string());   // FUN_0x400e69a4
        json_add_string(doc, "mac_address",     mac);
        json_add_string(doc, "hardware_subtype",HW_SUBTYPE);
        json_add_string(doc, "firmware_version",firmware_version_string());// FUN_0x400e6998
        json_add_bool  (doc, "registered",      device_is_registered());  // FUN_0x400e68f0
        json_add_bool  (doc, "owner_protected", !owner_is_authenticated(g_conn_handle));
        if (network_is_up() && wait_for_ip(10))        // FUN_0x400e6950 / FUN_0x400e9d34
            json_add_string(doc, "ip_sta", sta_ip_string());              // FUN_0x400e9ee8
    }
    else if (sub == SUB_DESIRED || sub == SUB_SHADOW_REPORTED) {  // 0x0B / 0x0D
        if (!owner_is_authenticated(g_conn_handle)) {
            ESP_LOGW(T_DEVINFO, "Shadow read (sub 0x%02x) before owner authentication", sub);
            return ATT_INSUFFICIENT_AUTH;
        }
        doc = (sub == SUB_SHADOW_REPORTED) ? shadow_reported_doc()    // FUN_0x400eddfc
                                           : shadow_desired_doc();    // FUN_0x400ede10
        if (!doc) { ESP_LOGW(T_DEVINFO, "Shadow sub 0x%02x is not available", sub); return ATT_READ_NOT_PERMITTED; }
    }
    else if (sub == SUB_EXPORT) {                      // 0x0F
        if (!owner_is_authenticated(g_conn_handle)) {
            ESP_LOGW(T_DEVINFO, "Export read before owner authentication");
            return ATT_INSUFFICIENT_AUTH;
        }
        doc = export_current_page();                   // FUN_0x400e68e4
        if (!doc) { ESP_LOGW(T_DEVINFO, "Export read with nothing to export"); return ATT_READ_NOT_PERMITTED; }
    }
    else {
        ESP_LOGW(T_DEVINFO, "Unknown sub-characteristic 0x%02x read", sub);
        return ATT_READ_NOT_PERMITTED;
    }

    char *txt = json_serialise(doc);                   // FUN_0x400e93d4
    if (!txt) { ESP_LOGE(T_DEVINFO, "Could not serialise JSON to string for sub 0x%02x", sub);
                json_free(doc); return ATT_INSUFFICIENT_RES; }
    int rc = gatt_read_reply(txt);                     // FUN_0x400edd80
    json_free(txt); json_free(doc);
    return rc;
}

// =============================================================================
// Direct command dispatcher   (FUN_0x400e63e8)
//
// cmd is matched by strcmp against each name. Returns 1 on success, 0 on refusal
// (refusals are logged "Direct command %s refused: %s" and reported back via
// command_refused()). args is the parsed "args" JSON object (may be null).
// =============================================================================
int command_dispatch(const char *cmd, const void *args)
{
    if (!strcmp(cmd, "keep_awake")) {
        keep_awake(120000);                            // FUN_0x400e4de0 — ~120 s gesture
        ESP_LOGI(T_PORT, "Direct command keep_awake: a gesture, awake for %u s", 0x78);
        return 1;
    }

    if (!strcmp(cmd, "start_automation")) {
        if (!args) return command_refused("start_automation: args must be the automation's name");
        char name[0x40];
        if (!json_copy_string(args, name, sizeof name)) // FUN_0x400d9b84
            return command_refused("start_automation: args must be the automation's name");
        ESP_LOGI(T_PORT, "Direct command start_automation: %s", name);
        automation_start(name);
        return 1;
    }
    if (!strcmp(cmd, "stop_automation")) {
        ESP_LOGI(T_PORT, "Direct command stop_automation");
        automation_stop();
        return 1;
    }
    if (!strcmp(cmd, "upload_log")) {
        ESP_LOGI(T_PORT, "Direct command upload_log");
        upload_log();
        return 1;
    }

    // calibration points: water uses gravity 1.000; wort needs an explicit gravity
    if (!strcmp(cmd, "start_calibration_water") || !strcmp(cmd, "start_calibration_wort")) {
        bool is_wort = !strcmp(cmd, "start_calibration_wort");
        double gravity = 1.0;                          // 0x3ff0000000000000
        if (is_wort) {
            const void *g = json_get(args, "gravity"); // FUN_0x400e862c
            if (!g)                    return command_refused("start_calibration_wort needs \"gravity\"");
            if (json_type(g) != 3 /*number*/)
                                        return command_refused("\"gravity\" is not a number");
            gravity = json_number(g);                  // clamped to a sane range
        }
        const char *err = nullptr;
        if (!calibration_point(is_wort, gravity, &err)) {  // FUN_0x400ddbe0
            ESP_LOGW(T_PORT, "Direct command %s refused: %s", cmd, err);
            return command_refused(err);
        }
        ESP_LOGI(T_PORT, "Direct command %s at %g", cmd, gravity);
        return 1;
    }
    if (!strcmp(cmd, "cancel_calibration")) {
        if (!calibration_cancel()) return command_refused("no calibration run in progress");
        ESP_LOGI(T_PORT, "Direct command cancel_calibration");
        return 1;
    }
    if (!strcmp(cmd, "clear_calibration")) {
        ESP_LOGI(T_PORT, "Direct command clear_calibration");
        calibration_clear();
        return 1;
    }

    if (!strcmp(cmd, "report_now")) {
        if (!device_is_registered())
            return command_refused("report_now: the device is not registered");
        ESP_LOGI(T_PORT, "Direct command report_now");
        report_now();
        return 1;
    }

    if (!strcmp(cmd, "set_dev_options"))
        return set_bool_setting(cmd, "dev_options", args);   // FUN_0x400e5a84

    if (!strcmp(cmd, "set_beta_updates")) {
        if (!set_bool_setting(cmd, "beta_updates", args)) return 0;
        // optional {"check": true} triggers an immediate beta update check
        if (args && json_get_bool(args, "check") == 1) {    // FUN_0x400e877c
            if (!beta_updates_check_now())                   // FUN_0x400e5d6c
                ESP_LOGW(T_PORT, "set_beta_updates: the setting is applied but the check could not run");
        }
        return 1;
    }

    if (!strcmp(cmd, "clear_wifi")) {
        if (command_guard_busy(cmd)) return 0;         // refused during update/calibration
        ESP_LOGW(T_PORT, "Direct command clear_wifi");
        wifi_forget();                                 // FUN_0x400e983c
        wifi_restart_sta();                            // FUN_0x400ea0d4
        return 1;
    }
    if (!strcmp(cmd, "clear_registration")) {
        if (command_guard_busy(cmd)) return 0;
        ESP_LOGW(T_PORT, "Direct command clear_registration");
        registration_clear();                          // FUN_0x400e128c
        return 1;
    }

    if (!strcmp(cmd, "factory_reset")) {
        // Requires args.mac == this device's own MAC (compared as 12 normalised
        // lowercase hex nibbles), as a safety interlock.
        const char *mac = args ? json_get_string(args, "mac") : nullptr;
        char want[13], got[13];
        own_mac_string(want);                          // FUN_0x400e69c4
        if (mac && normalise_mac(mac, got) && !strcmp(got, want)) {
            if (command_guard_busy(cmd)) return 0;
            ESP_LOGW(T_PORT, "Direct command factory_reset");
            factory_reset();                           // FUN_0x400e12a0 — erase NVS + restart
            return 1;
        }
        ESP_LOGW(T_PORT, "Direct command %s refused: %s", cmd,
                 mac ? "the mac is another device's" : "no mac given");
        return command_refused("%s needs \"mac\": this device's own address");
    }

    if (!strcmp(cmd, "export")) {
        // args: {"what": gesture_log|gravity_data|log, "page": <uint>=0..}
        uint32_t page = 0;
        const char *what = args ? json_get_string(args, "what") : nullptr;
        const void *p    = args ? json_get(args, "page") : nullptr;
        if (p) {
            if (json_type(p) != 3 || json_number(p) < 0 || json_number(p) > UINT32_MAX)
                return command_refused("export: \"page\" is a whole number from 0");
            page = (uint32_t)json_number(p);
        }
        if (!export_data(what, page))                  // FUN_0x400de9a8
            return command_refused("export: \"what\" is gesture_log, gravity_data or log");
        return 1;
    }

    // unknown
    char msg[0x60];
    snprintf(msg, sizeof msg, "unknown command \"%s\"", cmd);
    ESP_LOGW(T_PORT, "%s", msg);
    return command_refused(msg);
}

// =============================================================================
// GATT access callbacks — thin shims that extract the sub-char id for the
// connection and dispatch to the read/write routers above.
//   svc0000_cb @ 0x400ee218 (devinfo, chars 0004-0007)
//   svc0100_cb @ 0x400ee580 (wifi,    chars 0101-0105)
//   svc0200_cb @ 0x400edbc0 (reg,     chars 0201-0203)
// (Bodies in cmd_decomp_svc.txt / cmd_decomp.txt; they map a characteristic +
// read/write op to a sub-char id, then call ble_write_router / ble_read_router.)
// =============================================================================

// FUN_0x400ec254 — bluetooth_init(): create the start/stop work queue
// ("start_stop_queue != NULL"), register the GATT services, and start the GAP
// advertising state machine (FUN_0x400ecacc). (Not in the decompiled bundle;
// summarised from app_main + assertion strings.)
void bluetooth_init(void) { /* see FUN_0x400ec254 */ }
