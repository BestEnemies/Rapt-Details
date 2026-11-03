// registration.cpp — RAPT Pill 2.0.0, reverse-engineered (readable reconstruction)
//
// Confidence: High for the claim HTTP/JSON flow and the conn_req refcount;
// Medium for the auto-reg rate-limiting arithmetic (the day-counter math is
// faithful to the decompilation but the exact field meanings are inferred).
//
// The device "claims" itself against an AWS API Gateway endpoint; on success
// the backend returns an IoT thing name + certificate + private key, which are
// stored in NVS and then used by the AWS IoT (shadow/jobs) client.
#include "registration.h"
#include "nonvolatile.h"   // nonvolatile_get_string_dyn(), NVS key descriptors
#include "bluetooth.h"     // ble connected/connectable queries
#include "measurement.h"   // report_now / telemetry reload
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <stdlib.h>

static const char *AUTOREG = "autoreg";
static const char *CONNREQ = "conn_req";

// Claim endpoint (AWS API Gateway, ap-southeast-2). Full path truncated in the
// string table; base host confirmed.
#define CLAIM_URL "https://d78naeycz0.execute-api.ap-southeast-2.amazonaws.com/v1/devices/..."
#define CLAIM_BODY_MAX 0x100   // request JSON buffer
#define CLAIM_RESP_MAX 0x2800  // response buffer (0x27ff usable + NUL)

// Networking event-group bits (group "networking_event_group", DAT_400d1514).
// Set by the BLE command handler; consumed by networking_task().
#define EVT_CLEAR_REGISTRATION (1u << 2) // 0x4
#define EVT_FACTORY_RESET      (1u << 3) // 0x8

// ---- external helpers (named from evidence; defined elsewhere) --------------
extern "C" const char *device_mac_string(void);        // FUN_400e69c4
extern "C" const char *firmware_version_string(void);  // FUN_400e3278
extern "C" bool  network_is_connected(void);           // FUN_400e6a08 (*DAT_400d1ea8)
extern "C" bool  network_should_connect(void);         // FUN_400e69ac
extern "C" bool  update_check_in_progress(void);       // FUN_400e3294 / FUN_400e32b4
extern "C" size_t largest_free_block(int caps);        // FUN_400d5b38 -> FUN_400d5ac4
extern bool  nvs_get_registered_flag(void);            // FUN_400eb1d8(registered,0)

// =============================================================================
// is_pem @ 0x400da278
// Validate that a string looks like a PEM blob and is within length limits.
static bool is_pem(const char *s, const char *label)
{
    if (s == nullptr || *s == '\0' || strlen(s) > 0x0fff) {
        ESP_LOGE(AUTOREG, "%s missing or too long", label);
        return false;
    }
    if (strstr(s, "-----BEGIN ") && strstr(s, "-----END "))
        return true;
    ESP_LOGE(AUTOREG, "%s is not PEM", label);
    return false;
}

// =============================================================================
// registration_claim @ 0x400da2e0
// Build the claim request, POST/GET it to the AWS endpoint, parse the JSON
// answer and (on success) store thingName/certificatePem/privateKey in NVS.
claim_result registration_claim(void)
{
    const char *mac = device_mac_string();
    const char *fw  = firmware_version_string();
    const char *registered = nvs_get_registered_flag() ? "true" : "false";

    char body[CLAIM_BODY_MAX];
    int n = snprintf(body, sizeof body,
        "{\"deviceType\":\"%s\",\"macAddress\":\"%s\",\"firmwareVersion\":\"%s\","
        "\"hardwareVersion\":\"%s\",\"registered\":%s}",
        "PillHydrometer", mac, fw, "1.0", registered);
    if (n >= CLAIM_BODY_MAX) {
        ESP_LOGE(AUTOREG, "Request too long");
        return CLAIM_ERROR;
    }

    char *resp = (char *)malloc(CLAIM_RESP_MAX);
    if (resp == nullptr) {
        ESP_LOGE(AUTOREG, "Out of memory for the response");
        return CLAIM_ERROR;
    }
    claim_result result = CLAIM_ERROR;

    esp_http_client_config_t cfg = {};
    cfg.url = CLAIM_URL;
    cfg.timeout_ms = 15000;
    cfg.is_async = false; /* +0x27 = 1 in the struct; meaning inferred */

    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == nullptr) {
        ESP_LOGE(AUTOREG, "esp_http_client_init() failed");
        free(resp);
        return CLAIM_ERROR;
    }

    esp_http_client_set_header(c, "Content-Type", "application/json");
    ESP_LOGI(AUTOREG, "Claim GET %s %s", CLAIM_URL, body);

    esp_err_t e = esp_http_client_open(c, (int)strlen(body));
    if (e != ESP_OK) {
        ESP_LOGE(AUTOREG, "esp_http_client_open() failed: %d %s", e, body);
        goto cleanup;
    }

    if ((size_t)esp_http_client_write(c, body, (int)strlen(body)) != strlen(body)) {
        ESP_LOGE(AUTOREG, "esp_http_client_write() failed");
        esp_http_client_close(c);
        goto cleanup;
    }

    if (esp_http_client_fetch_headers(c) < 0) {
        ESP_LOGE(AUTOREG, "esp_http_client_fetch_headers() failed");
        esp_http_client_close(c);
        goto cleanup;
    }

    {
        int status = esp_http_client_get_status_code(c);
        int got = 0, r;
        while ((r = esp_http_client_read(c, resp + got, (CLAIM_RESP_MAX - 1) - got)) > 0) {
            got += r;
            if (got > CLAIM_RESP_MAX - 1) break;
        }
        resp[got] = '\0';
        esp_http_client_close(c);

        if (r < 0) { ESP_LOGE(AUTOREG, "esp_http_client_read() failed"); goto cleanup; }
        if (status != 200) {
            ESP_LOGI(AUTOREG, "Claim HTTP %d: %.200s", status, resp);
            goto cleanup;
        }
        if (got > CLAIM_RESP_MAX - 1) { ESP_LOGE(AUTOREG, "Response too large"); goto cleanup; }
        ESP_LOGI(AUTOREG, "Claim HTTP %d (%d bytes)", 200, got);

        // --- parse JSON -----------------------------------------------------
        json_t *root = json_parse(resp);              // FUN_400e91f8
        if (root == nullptr || json_type(root) != JSON_OBJECT) {
            ESP_LOGE(AUTOREG, "Response is not a JSON object");
            if (root) json_free(root);
            goto cleanup;
        }

        int registered_field = json_get_bool(root, "registered"); // FUN_400e877c, -1 if absent
        const char *reason    = json_get_string(root, "reason");   // FUN_400e871c
        if (registered_field == -1) {
            ESP_LOGE(AUTOREG, "Response has no registered field");
        } else if (registered_field == 0) {
            ESP_LOGI(AUTOREG, "Backend declined automatic registration (%.48s)",
                     reason ? reason : "no reason given");
            result = CLAIM_DECLINED;
        } else if (json_get_bool(root, "dryRun") == 1) {
            ESP_LOGE(AUTOREG, "Answer is a dry run");
        } else {
            const char *ans_mac = json_get_string(root, "macAddress");
            if (ans_mac == nullptr) {
                ESP_LOGE(AUTOREG, "Answer names no device");
            } else if (strncmp(ans_mac, device_mac_string(),
                               strlen(device_mac_string())) != 0 ||
                       (ans_mac[strlen(device_mac_string())] != '\0' &&
                        ans_mac[strlen(device_mac_string())] != '_')) {
                ESP_LOGE(AUTOREG, "Answer is for another device (%.32s)", ans_mac);
            } else {
                const char *thing = json_get_string(root, "thingName");
                const char *cert  = json_get_string(root, "certificatePem");
                const char *key   = json_get_string(root, "privateKey");
                if (thing == nullptr || *thing == '\0' || strlen(thing) > 0x7f) {
                    ESP_LOGE(AUTOREG, "thingName missing or too long");
                } else if (is_pem(cert, "certificatePem") && is_pem(key, "privateKey")) {
                    if (nvs_set_string(KEY_CERT_PEM, cert) &&     // FUN_400eb63c
                        nvs_set_string(KEY_PRIVATE_KEY, key) &&
                        nvs_set_string(KEY_THING_NAME, thing)) {
                        aws_iot_credentials_changed();            // FUN_400f11cc
                        ESP_LOGI(AUTOREG, "Credentials stored (thing name %u chars, %.48s)",
                                 (unsigned)strlen(thing),
                                 reason ? reason : "no reason given");
                        // Optional firmware hint — OTA module does its own check.
                        json_t *fwobj = json_get_object(root, "firmware"); // FUN_400e8670
                        if (fwobj && json_get_bool(fwobj, "updateAvailable") == 1) {
                            const char *tv = json_get_string(fwobj, "targetVersion");
                            ESP_LOGI(AUTOREG,
                                "Firmware %.31s on offer; the OTA module's own check takes ...",
                                tv ? tv : "");
                        }
                        result = CLAIM_OK;
                    } else {
                        ESP_LOGE(AUTOREG, "Could not store the credentials; discarding them");
                        nvs_erase_key(KEY_THING_NAME);   // FUN_400eb8ec
                        nvs_erase_key(KEY_PRIVATE_KEY);
                        nvs_erase_key(KEY_CERT_PEM);
                    }
                }
            }
            if (result != CLAIM_OK) result = CLAIM_ERROR;
        }
        json_free(root);
    }

cleanup:
    esp_http_client_cleanup(c);
    memset(resp, 0, CLAIM_RESP_MAX);
    free(resp);
    return result;
}

// =============================================================================
// autoreg_give_up @ 0x400da248 — permanently stop trying this session.
static void autoreg_give_up(const char *reason)
{
    g_autoreg.credentials_pending = true; // DAT_400d09d0 = 1 /* inferred */
    g_autoreg.state = AUTOREG_DONE;       // DAT_400d09d4 = 2
    ESP_LOGI(AUTOREG, "No automatic registration: %s", reason);
}

// autoreg_set_reason @ 0x400da738 — log the current blocking reason once.
static void autoreg_set_reason(const char *reason)
{
    if (g_autoreg.last_reason != reason) {        // DAT_400d0ab4
        g_autoreg.last_reason = reason;
        ESP_LOGI(AUTOREG, "Automatic registration waiting on %s", reason);
    }
}

// autoreg_precheck @ 0x400da214
// True when auto-registration need not run: not enabled, or already connected.
// (Field meanings inferred from call sites; kept faithful to the logic.)
static bool autoreg_not_needed(void)
{
    if (!g_autoreg.migrated_flag)   return false;   // DAT_400d09cc
    if (network_is_connected())     return false;   // FUN_400e6a08 != 0
    if (!wifi_credentials_present()) return false;  // FUN_400e9a14 == 0
    return !soft_ap_running();                      // FUN_400eaef4
}

// =============================================================================
// automatic_registration_tick @ 0x400da854
// Drives the AUTOREG state machine once per networking-task loop.
void automatic_registration_tick(void)
{
    if (!g_autoreg.enabled) return;             // DAT_400d0abc

    if (g_autoreg.state == AUTOREG_WAIT_FIRST_CONNECTION) {
        if (network_is_connected()) {
            ESP_LOGI(AUTOREG, "Registered automatically ...");
            g_autoreg.state = AUTOREG_DONE;
        } else {
            // Credentials stored but not connected: warn, then drop the BLE
            // link so the phone lets the Pill join wifi. Rate-limited to 2 s.
            if ((esp_timer_now_ms() - g_autoreg.wait_since) < 2000) return;
            ESP_LOGW(AUTOREG, "Credentials stored but not connected yet; releasing the BLE ...");
            g_autoreg.state = AUTOREG_DONE;
        }
        return;
    }
    if (g_autoreg.state != AUTOREG_IDLE) return;
    if (g_autoreg.credentials_pending) return;  // DAT_400d09d0

    g_autoreg.migrated_flag = nvs_get_registered_flag(); // refresh DAT_400d09cc

    if (autoreg_not_needed()) {
        const char *why = "not a Pill migrated from the Azure-era firmware";
        if (g_autoreg.migrated_flag) {
            why = network_is_connected()
                ? "already registered"
                : "no saved network, or on the factory access point";
        }
        autoreg_give_up(why);
        return;
    }

    // Already have a stored thing name? Then just wait for first connect.
    char *thing = nonvolatile_get_string_dyn(KEY_THING_NAME);  // FUN_400eb590
    bool have_thing = (thing && *thing);
    free(thing);
    if (have_thing) {
        autoreg_give_up("credentials already stored, waiting for their first connection");
        return;
    }

    // Once-per-day cap (wall-clock day counter, 64-bit).
    int64_t now = wall_clock_day();                 // FUN_400d081c(0)
    if (now >= g_autoreg.day_floor &&               // DAT_400d0aec/0af0
        g_autoreg.last_day <= now &&
        same_day(now, g_autoreg.last_day, g_autoreg.day_window /*DAT_400d0af4*/)) {
        autoreg_give_up("already asked today");
        return;
    }

    // Session wait window (~0x1194 ticks ≈ 45 s) and resource gates.
    if ((esp_timer_now_ms() - g_autoreg.session_start) < 0x1194) {
        const char *why;
        if (!network_should_connect())            why = "the network";
        else if (update_check_in_progress())      why = "the update check";
        else if (ble_is_connected())              why = "the phone to disconnect"; // FUN_400ec6e4
        else if (largest_free_block(MALLOC_CAP_DEFAULT) < 0x6800)
                                                  why = "heap for a TLS session";
        else why = nullptr;

        if (why) { autoreg_set_reason(why); return; }

        g_autoreg.credentials_pending = true;     // claim in progress
        g_autoreg.last_day = now;                 // record the attempt day

        claim_result r = registration_claim();
        if (r != CLAIM_OK) {
            if (r != CLAIM_DECLINED)
                ESP_LOGW(AUTOREG, "Claim failed, the app ... (%u)", 0x18);
            g_autoreg.state = AUTOREG_DONE;
            return;
        }
        g_autoreg.wait_since = esp_timer_now_ms();
        g_autoreg.last_connect_check = esp_timer_now_ms();
        g_autoreg.have_credentials = true;        // DAT_400d0ac4
        g_autoreg.state = AUTOREG_WAIT_FIRST_CONNECTION;
        return;
    }

    autoreg_give_up(g_autoreg.last_reason
                    ? g_autoreg.last_reason
                    : "conditions not met within the session's wait");
}

// =============================================================================
// Connection-request refcount (conn_req). Two nested refcounts:
//   g_conn.refs       — wifi-manager lifetime
//   g_conn.ap_clients — soft-AP visibility
// A FreeRTOS mutex (g_conn.lock) serialises the whole thing.
// -----------------------------------------------------------------------------

// conn_req_acquire_locked @ 0x400de55c — first acquirer starts the manager.
static void conn_req_acquire_locked(void)
{
    if (g_conn.refs == 0) {
        wifi_manager_start();                           // FUN_400e9dc8
        wifi_manager_set_callback(WM_EVENT_STA_DISCONNECTED, on_sta_disconnected); // 0x0c
        wifi_manager_set_callback(WM_EVENT_STA_GOT_IP,       on_sta_got_ip);       // 0x0a
        wifi_manager_set_callback(WM_ORDER_START_AP,         on_ap_started);       // 0x09
        wifi_manager_set_http_callback(1, http_cb_get);     // FUN_400e9668
        wifi_manager_set_http_callback(3, http_cb_post);
    }
    g_conn.refs++;
}

// conn_req_release_locked @ 0x400de5fc — last releaser destroys the manager.
static void conn_req_release_locked(void)
{
    if (--g_conn.refs == 0) {
        wifi_manager_destroy();                         // FUN_400ea3b8
        g_conn.ap_forced_down = false;                  // DAT_400d11dc
        set_ble_connectable_wanted(false);              // FUN_400e6a20(0)
    }
}

// conn_req_acquire @ 0x400de654
void conn_req_acquire(void)
{
    if (xSemaphoreTake(g_conn.lock, 500) == 0) {
        ESP_LOGE(CONNREQ, "Could not take semaphore ...");
        return;
    }
    conn_req_acquire_locked();
    xSemaphoreGive(g_conn.lock);
}

// conn_req_release @ 0x400de690
void conn_req_release(void)
{
    if (xSemaphoreTake(g_conn.lock, 500) == 0) {
        ESP_LOGE(CONNREQ, "Could not take semaphore ...");
        return;
    }
    conn_req_release_locked();
    xSemaphoreGive(g_conn.lock);
}

// soft_ap_take_down @ 0x400de724 — called when a client connects by BLE, so the
// phone stops seeing the config AP. Stops the AP and marks it forced-down.
void soft_ap_take_down(void)
{
    if (xSemaphoreTake(g_conn.lock, 500) == 0) {
        ESP_LOGE(CONNREQ, "Could not take semaphore ...");
        return;
    }
    if (g_conn.ap_clients > 0 && !g_conn.ap_forced_down) {
        wifi_manager_stop_ap();                         // FUN_400eaec4
        g_conn.ap_forced_down = true;
    }
    xSemaphoreGive(g_conn.lock);
}

// soft_ap_is_wanted @ 0x400de778 — true if an AP is requested and not forced down.
bool soft_ap_is_wanted(void)
{
    bool wanted = false;
    if (xSemaphoreTake(g_conn.lock, 500) == 0) {
        ESP_LOGE(CONNREQ, "Could not take semaphore ...");
        return false;
    }
    if (g_conn.ap_clients > 0)
        wanted = !g_conn.ap_forced_down;
    xSemaphoreGive(g_conn.lock);
    return wanted;
}

// on_soft_ap_up @ 0x400de59c — captive-portal startup log.
static void on_soft_ap_up(void)
{
    ESP_LOGI(CONNREQ, "Soft AP up, SSID %s / %s, gateway %s",
             FACTORY_AP_SSID, FACTORY_AP_PASSWORD, FACTORY_AP_GATEWAY);
}

// Factory-AP credential accessors (@0x400eaee4 / @0x400eaeec).
const char *factory_ap_ssid(void)     { return FACTORY_AP_SSID; }
const char *factory_ap_password(void) { return FACTORY_AP_PASSWORD; }

// =============================================================================
// networking_task @ 0x400e1a90
// The top-level supervisor: drains the networking event group (factory-reset /
// clear-registration commands), reconciles BLE-vs-AP exclusivity, ticks
// auto-registration, and starts/stops the AWS IoT client as connectivity
// demands. Runs forever on a 10 ms cadence.
void networking_task(void * /*arg*/)
{
    bool iot_started = false;
    for (;;) {
        uint32_t bits = xEventGroupClearBits(g_net_event_group,
                                             EVT_CLEAR_REGISTRATION | EVT_FACTORY_RESET);

        if (bits & EVT_FACTORY_RESET) {
            ESP_LOGW(TAG_MAIN, "Factory reset requested over BLE: erasing NVS and restarting");
            vTaskDelay(100 / portTICK_PERIOD_MS);
            nvs_erase_all();           // FUN_400eb104
            esp_restart();             // FUN_400d4a58
        }
        if (bits & EVT_CLEAR_REGISTRATION) {
            ESP_LOGW(TAG_MAIN, "Registration cleared over BLE");
            aws_iot_credentials_changed();        // FUN_400f11e4 (drop thing/session)
            nvs_erase_key_cached(KEY_CERT_PEM);   // FUN_400eb8ec(*DAT_400d0c18)
            nvs_set_registered_flag(false);       // FUN_400eb290(registered, 0)
        }

        service_networking_state();               // FUN_400e13f0

        // If a station joined our AP while BLE is connectable, drop BLE.
        if (ap_client_count() > 0 && ble_is_connectable()) {         // FUN_400eae70 / FUN_400ec1fc(2)
            ESP_LOGI(TAG_MAIN, "Someone connected via. AP, taking down BLE connectable");
            ble_stop_connectable();               // FUN_400ec090(2)
        }
        // If a client connected by BLE while an AP is wanted, drop the AP.
        if (ble_is_connected() && soft_ap_is_wanted()) {             // FUN_400ec6e4
            ESP_LOGI(TAG_MAIN, "Someone connected via. BLE, taking down AP");
            soft_ap_take_down();
        }

        automatic_registration_tick();

        // Start/stop the AWS IoT (shadow/jobs) client as connectivity dictates.
        if (!network_is_connected()) {
            if (network_should_connect() && !iot_started) {
                aws_iot_start();                  // FUN_400f1100
                iot_started = true;
            }
        } else if (!network_should_connect() /*==1*/ && iot_started) {
            aws_iot_stop();                       // FUN_400f1120
            iot_started = false;
        }

        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}
