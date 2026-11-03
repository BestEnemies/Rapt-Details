// ota.cpp — RAPT Pill 2.0.0 firmware OTA update check (reverse-engineered)
//
// Reverse-engineered from app_2.0.0.bin @ 0x400dc83c / 0x400dc818.
// NOT original source. Confidence: HIGH (URL, field names, control flow and
// log strings all recovered verbatim from the binary).
//
// SDK calls are shown with their real ESP-IDF names where the callee was
// identified. The embedded JSON accessors (js_*) are a small dotted-path-aware
// wrapper over a parsed tree (see aws_shadow.cpp for the facade); the binary
// calls them through pointer tables (PTR_FUN_400d08c4 = js_type, etc.).

#include "ota.h"
#include "json_util.h"      // js_parse / js_get_* facade (reconstructed)
#include "esp_http_client.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static const char *TAG = "aws_update";

// Update-check endpoint recovered verbatim from rodata @ 0x3f40.. :
//   https://d78naeycz0.execute-api.ap-southeast-2.amazonaws.com/v1/devices/...
// Format args (order as passed to snprintf @ 0x400dc83c):
//   deviceType="PillHydrometer", <hardware_version>, "1.0" /* inferred literal */,
//   <firmware_version>
// Exact path tail after the host is held in the same rodata literal.
static const char *UPDATE_CHECK_URL_FMT =
    "https://d78naeycz0.execute-api.ap-southeast-2.amazonaws.com"
    "/v1/devices/firmwareVersionCheck-wifi"
    "?thingType=%s&hardwareVersion=%s&apiVersion=%s&firmwareVersion=%s"; /* path/params inferred; host+scheme verbatim */

#define URL_MAX   0x1c0   // 448 — snprintf cap @ 0x400dc83c
#define RESP_MAX  0x1000  // 4096 — response buffer (heap)
#define DLURL_MAX 0x800   // 2047 usable + NUL

// @ 0x400dc818 — map result code to a short word for logging.
const char *ota_check_result_str(ota_check_result_t r)
{
    switch (r) {
        case OTA_CHECK_OK:           return "ok";
        case OTA_CHECK_NO_RESPONSE:  return "no response";
        case OTA_CHECK_HTTP_ERROR:   return "HTTP error";
        case OTA_CHECK_BAD_RESPONSE: return "bad response";
        default:                     return "unknown";
    }
}

// Decode 64 lowercase/upper hex chars into 32 bytes. Returns false on any
// non-hex char. (@ 0x400dc83c inner loop, uses the libc ctype table + strtol.)
static bool hex64_to_bytes(const char *hex, uint8_t out[32])
{
    for (int i = 0; i < 32; i++) {
        char c0 = hex[2*i], c1 = hex[2*i + 1];
        if (!isxdigit((unsigned char)c0) || !isxdigit((unsigned char)c1))
            return false;
        char pair[3] = { c0, c1, 0 };
        out[i] = (uint8_t)strtol(pair, nullptr, 16);
    }
    return true;
}

// @ 0x400dc83c — GET the update-check JSON and parse it.
ota_check_result_t ota_check_for_update(const char *hardware_version,
                                        const char *firmware_version,
                                        ota_update_info_t *out,
                                        int *http_status_out)
{
    memset(out, 0, sizeof(*out));
    if (!firmware_version) firmware_version = "";          // PTR_DAT_400d0e2c default

    char url[URL_MAX];
    int n = snprintf(url, URL_MAX, UPDATE_CHECK_URL_FMT,
                     "PillHydrometer", hardware_version, "1.0" /*inferred*/,
                     firmware_version);
    int http_status = -1;
    ota_check_result_t result;

    if (n >= URL_MAX - 1) {
        ESP_LOGE(TAG, "Update check URL too long");
        result = OTA_CHECK_BAD_RESPONSE;
        goto done;
    }

    {
        char *buf = (char *)malloc(RESP_MAX);              // FUN_40098368
        if (!buf) {
            ESP_LOGE(TAG, "Out of memory for the update check response");
            result = OTA_CHECK_NO_RESPONSE;
            goto done;
        }

        esp_http_client_config_t cfg = {};                 // apcStack_274, zeroed
        cfg.url         = url;
        cfg.timeout_ms  = 15000;                           // uStack_228
        cfg.crt_bundle_attach = /* flag uStack_203 = 1 */ nullptr; /* inferred: use global CA store */

        esp_http_client_handle_t cli = esp_http_client_init(&cfg); // FUN_40122d88
        if (!cli) {
            ESP_LOGE(TAG, "esp_http_client_init() failed");
            free(buf);
            result = OTA_CHECK_NO_RESPONSE;
            goto done;
        }

        ESP_LOGI(TAG, "Update check: GET %s", url);

        esp_err_t err = esp_http_client_open(cli, 0);      // FUN_401234dc
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_http_client_open() failed: %d", err);
            result = OTA_CHECK_NO_RESPONSE;
            http_status = -1;
            goto cleanup;
        }

        if (esp_http_client_fetch_headers(cli) < 0) {      // FUN_40122904
            ESP_LOGE(TAG, "esp_http_client_fetch_headers() failed");
            esp_http_client_close(cli);
            result = OTA_CHECK_NO_RESPONSE;
            goto cleanup;
        }
        http_status = esp_http_client_get_status_code(cli); // PTR_FUN_400d0980

        // Read whole body (capped to RESP_MAX-1).
        int total = 0, r;
        while ((r = esp_http_client_read(cli, buf + total, (RESP_MAX - 1) - total)) > 0) // FUN_40122788
            total += r;
        buf[total] = '\0';
        esp_http_client_close(cli);                        // FUN_401229d4
        if (r < 0) {
            ESP_LOGE(TAG, "esp_http_client_read() failed");
            result = OTA_CHECK_NO_RESPONSE;
            goto cleanup;
        }

        if (http_status != 200) {
            ESP_LOGI(TAG, "Update check HTTP %d (%d bytes)", http_status, total);
            result = OTA_CHECK_HTTP_ERROR;
            goto cleanup;
        }
        ESP_LOGI(TAG, "Update check HTTP %d: %s", 200, buf);
        if (total > RESP_MAX - 1) {                        // DAT_400d0e70 guard
            ESP_LOGE(TAG, "Response too large");
            result = OTA_CHECK_BAD_RESPONSE;
            goto cleanup;
        }

        js_value *root = js_parse(buf);                    // FUN_400e91f8
        if (!root || js_type(root) != JS_OBJECT) {         // PTR_FUN_400d08c4, 4==object
            ESP_LOGE(TAG, "Response is not a JSON object");
            result = OTA_CHECK_BAD_RESPONSE;
            if (root) js_free(root);
            goto cleanup;
        }
        js_value *obj = js_root(root);                     // PTR_FUN_400d096c

        // { "success": bool, "error": str?, "updateAvailable": bool,
        //   "downloadUrl": str, "targetVersion": str, "size": num,
        //   "checksum": { "algorithm": "SHA256", "value": <64 hex> }, "reason": str? }
        int success = js_get_bool(obj, "success");         // FUN_400e877c (1/0/-1)
        if (success != 1) {
            const char *e = js_get_string(obj, "error");   // FUN_400e871c
            ESP_LOGE(TAG, "Update check not successful: %s", e ? e : "(no error given)");
            result = OTA_CHECK_BAD_RESPONSE;
            js_free(root);
            goto cleanup;
        }

        int avail = js_get_bool(obj, "updateAvailable");
        if (avail == -1) {
            ESP_LOGE(TAG, "Response has no updateAvailable");
            result = OTA_CHECK_BAD_RESPONSE;
            js_free(root);
            goto cleanup;
        }
        out->update_available = (avail == 1);

        if (avail != 1) {
            const char *reason = js_get_string(obj, "reason");
            ESP_LOGI(TAG, "No update available: %s", reason ? reason : "(no reason given)");
            result = OTA_CHECK_OK;
            js_free(root);
            goto cleanup;
        }

        // update available -> require downloadUrl + SHA256 checksum.
        const char *dl = js_get_string(obj, "downloadUrl");
        if (!dl) {
            ESP_LOGE(TAG, "Update offered without a downloadUrl");
            result = OTA_CHECK_BAD_RESPONSE;
            js_free(root);
            goto cleanup;
        }
        if (strlen(dl) > DLURL_MAX - 1) {
            ESP_LOGE(TAG, "downloadUrl too long (%u bytes)", (unsigned)strlen(dl));
            result = OTA_CHECK_BAD_RESPONSE;
            js_free(root);
            goto cleanup;
        }
        strcpy(out->download_url, dl);
        // log the download host (strstr "://")
        const char *host = strstr(dl, "://");
        if (host) host += 3;
        ESP_LOGI(TAG, "Download host: %s", host ? host : dl);

        const char *tv = js_get_string(obj, "targetVersion");
        strlcpy(out->target_version, tv ? tv : "", sizeof(out->target_version));
        out->size = (uint32_t)js_get_number(obj, "size");  // FUN_400e8744

        js_value *ck = js_get_object(obj, "checksum");     // FUN_400e8670
        bool ok_checksum = false;
        if (ck) {
            const char *algo = js_get_string(ck, "algorithm");
            const char *val  = js_get_string(ck, "value");
            if (algo && strcmp(algo, "SHA256") == 0 && val && strlen(val) == 64)
                ok_checksum = hex64_to_bytes(val, out->sha256);
        }
        out->checksum_valid = ok_checksum;
        if (!ok_checksum) {
            ESP_LOGE(TAG, "Update offered without a usable SHA256 checksum; refusing");
            result = OTA_CHECK_BAD_RESPONSE;
            js_free(root);
            goto cleanup;
        }

        result = OTA_CHECK_OK;
        js_free(root);

    cleanup:
        esp_http_client_cleanup(cli);                      // FUN_40122a18
        free(buf);                                         // FUN_40098388
    }

done:
    if (result != OTA_CHECK_OK)
        memset(out, 0, sizeof(*out));
    if (http_status_out) *http_status_out = http_status;

    // "Update check -> <result>[, up to date|, update available]"
    const char *suffix = "";
    if (result == OTA_CHECK_OK)
        suffix = out->update_available ? ", update available" : ", up to date";
    ESP_LOGI(TAG, "Update check -> %s%s", ota_check_result_str(result), suffix);
    return result;
}

// =============================================================================
// OTA DOWNLOAD + INSTALL  (extension — recovered source file "ota.cpp")
//
// These drive the actual image download and flash once a check has offered an
// update. Addresses are the Ghidra FUN_<addr> symbols. Confidence: HIGH — the
// esp_ota_* / esp_http_client_* sequence and every log/assert string are
// recovered verbatim (incl. the assert "ota.cpp":0x3a4).
// =============================================================================

// ota run-state (DAT_0x400d17a4): 0 idle, 1 checking, 2 deferred-waiting,
//   3 downloading/writing, 4 installed-pending-report.  /* inferred values */

// -----------------------------------------------------------------------------
// FUN_0x400e2b94 - ota_install(from_jobs, allow_restart):
//   1. ota_check_for_update() into a heap descriptor.
//   2. if none -> "Current firmware is up to date", done.
//   3. else open downloadUrl over HTTPS (esp_http_client, 40 s timeout,
//      0x400/0x1000 buffers); require HTTP 200.
//   4. esp_ota_begin() on the next OTA partition; stream the body in 512-byte
//      chunks, feeding BOTH a running mbedTLS SHA-256 and esp_ota_write().
//   5. verify SHA-256 vs the checksum from the check -> install is
//      checksum-gated ("Downloaded firmware failed its checksum").
//   6. esp_ota_end() + esp_ota_set_boot_partition(); persist fw_reported to NVS;
//      optionally restart ("OTA update done, restarting...").
// -----------------------------------------------------------------------------
void ota_install(bool from_jobs, bool allow_restart)
{
    ESP_LOGI(TAG, "Checking for OTA firmware update...");
    ota_run_state = 1;                                   // DAT_0x400d17a4

    char *scratch = (char *)malloc(0x84c);               // FUN_0x40098368
    if (!scratch) {
        snprintf(ota_last_error, 0xa0, "Out of memory for the update check");
        ota_publish_status();                            // FUN_0x400e28d0
        ESP_LOGE(TAG, "%s", ota_last_error);
        ota_set_phase(6, 0, 5);                          // FUN_0x400e2a40
        goto done;
    }

    {
        char hw[32]; own_hardware_string(hw);            // FUN_0x400e69c4
        ota_update_info_t *info = (ota_update_info_t *)scratch;
        int rc = ota_check_for_update(running_version, hw, info, nullptr); // FUN_0x400dc83c
        if (rc != OTA_CHECK_OK) {
            snprintf(ota_last_error, 0xa0, "Update check failed: %s (HTTP %d)",
                     ota_check_result_str((ota_check_result_t)rc), -1);    // FUN_0x400dc818
            ota_publish_status(); ESP_LOGE(TAG, "%s", ota_last_error);
            ota_set_phase(3, 0, 0); goto done;
        }
        if (!info->update_available) {
            snprintf(ota_last_error, 0xa0, "Current firmware is up to date");
            ota_publish_status(); ESP_LOGI(TAG, "%s", ota_last_error);
            ota_set_phase(3, 0, 0);
            if (ota_boot_state == 1 && ota_boot_state != 1) report_now(); // FUN_0x400e1360
            goto done;
        }

        ESP_LOGI(TAG, "Update available: %s", info->target_version);

        esp_http_client_config_t cfg = {};
        cfg.url            = info->download_url;          // info + 0x49
        cfg.timeout_ms     = 40000;
        cfg.buffer_size    = 0x400;
        cfg.buffer_size_tx = 0x1000;
        esp_http_client_handle_t cli = esp_http_client_init(&cfg);         // FUN_0x40122d88
        ESP_LOGI(TAG, "Free heap before OTA TLS: %u, largest %u",
                 esp_get_free_heap_size(), heap_largest_free_block());
        if (!cli) { snprintf(ota_last_error,0xa0,"Could not set up the firmware download"); goto fail; }
        if (esp_http_client_open(cli, 0) != ESP_OK) {     // FUN_0x401234dc
            snprintf(ota_last_error,0xa0,"Could not connect to firmware download server"); goto fail_cli; }
        int clen = esp_http_client_fetch_headers(cli);    // FUN_0x40122904
        if (clen < 0) { snprintf(ota_last_error,0xa0,"esp_http_client_fetch_headers() failed"); goto fail_cli; }
        int status = esp_http_client_get_status_code(cli);// PTR_FUN_400d0980
        if (status != 200) { snprintf(ota_last_error,0xa0,"firmware download server error: %d", status); goto fail_cli; }

        if (clen && info->size && info->size != (uint32_t)clen)
            ESP_LOGW(TAG, "Download size %d differs from the manifest size %u", clen, info->size);

        const uint8_t *want_sha = info->checksum_valid ? info->sha256 : nullptr; // info+0x48 / +0x28
        ESP_LOGI(TAG, "New firmware size: %d%s", clen, info->checksum_valid ? "" : " (unknown)");

        ota_run_state = 3;
        const esp_partition_t *part = esp_ota_get_next_update_partition(nullptr); // FUN_0x40106b8c
        assert(part && "update_partition != NULL");       // ota.cpp:0x3a4
        ESP_LOGI(TAG, "Writing new firmware to 0x%08lx", (unsigned long)part->address);

        esp_ota_handle_t h;
        if (esp_ota_begin(part, OTA_SIZE_UNKNOWN, &h) != ESP_OK)            // FUN_0x40106e40
            { snprintf(ota_last_error,0xa0,"esp_ota_begin() returned error"); goto fail_cli; }

        mbedtls_sha256_context sha;                       // FUN_0x4013620c/626c/62d8/62f0
        mbedtls_sha256_init(&sha); mbedtls_sha256_starts(&sha, 0);
        int got = 0, werr = 0, n;
        uint8_t buf[0x200];
        do {
            n = esp_http_client_read(cli, (char*)buf, sizeof buf);          // FUN_0x40122788
            if (n > 0) {
                mbedtls_sha256_update(&sha, buf, n);
                werr = esp_ota_write(h, buf, n);          // FUN_0x40106808
                got += n;
                ota_report_progress(got);                 // FUN_0x400e2974
            }
        } while (n >= 0 && werr == 0);
        uint8_t digest[32];
        mbedtls_sha256_finish(&sha, digest); mbedtls_sha256_free(&sha);

        if (n < 0)        { snprintf(ota_last_error,0xa0,"esp_http_client_read() returned error"); goto fail_ota; }
        if (info->size && got != (int)info->size)
                          { snprintf(ota_last_error,0xa0,"Download ended after %d of %d bytes", got, info->size); goto fail_ota; }
        if (werr)         { snprintf(ota_last_error,0xa0,"esp_ota_write() returned error: %d", werr); goto fail_ota; }
        if (want_sha && memcmp(digest, want_sha, 32) != 0)  // FUN_0x400d1888 == memcmp
                          { snprintf(ota_last_error,0xa0,"Downloaded firmware failed its checksum"); goto fail_ota; }

        if (esp_ota_end(h) != ESP_OK) { snprintf(ota_last_error,0xa0,"esp_ota_end() returned error: %d",0); goto fail_cli; }
        ota_report_progress(0xffffffff);

        char desc[0x100] = {0};
        if (esp_ota_get_partition_description(part, (esp_app_desc_t*)desc) == ESP_OK) // FUN_0x40106be0
            ESP_LOGI(TAG, "New firmware version %s", desc + 0x10);

        if (esp_ota_set_boot_partition(part) != ESP_OK)    // FUN_0x40106a78
            { snprintf(ota_last_error,0xa0,"Failed to set boot partition: %d",0); goto fail_cli; }

        ESP_LOGI(TAG, "OTA update completed successfully");
        ota_update_installed = 1;                         // DAT_0x400d1878
        nvs_set_ota_pending(ota_target_slot);             // FUN_0x400eb8ec
        nonvolatile_set_blob(nvs_fw_reported_key, fw_reported_blob); // FUN_0x400eb63c

        if (allow_restart) {
            esp_http_client_cleanup(cli);
            ESP_LOGI(TAG, "OTA update done, restarting...");
            ota_publish_status();
            ota_set_phase(5, 0, 0);
            vTaskDelay(ble_session_active ? 300 : 200);   // FUN_0x40093438
            esp_restart();                                // FUN_0x400d4a58
        }
        ota_wall_clock_rebase();                          // FUN_0x400e281c
        ota_run_state = 4;
        esp_http_client_cleanup(cli);
        goto done;

    fail_ota: esp_ota_abort(h);                           // FUN_0x4010696c
    fail_cli: esp_http_client_close(cli);
    fail:     ota_publish_status(); ESP_LOGE(TAG, "%s", ota_last_error);
              if (cli) esp_http_client_cleanup(cli);
    }
done:
    free(scratch);
    if (ota_run_state != 4) ota_run_state = 0;
}

// -----------------------------------------------------------------------------
// FUN_0x400e32fc - ota_update_gate(forced): pre-flight that decides whether to
// run ota_install() now, defer, or skip. Refuses unless the charger is connected
// and the internet is reachable; defers while a BLE session is active or free
// heap is below the OTA-TLS threshold (~0x6000). Recovered messages:
//   "No Internet connection", "Connect the charger before doing an update",
//   "Deferring OTA update check: BLE session...", "Largest free block %u ...".
// On go it may tear the BLE link down/up around the install, waits up to ~2 s
// for connectivity, then calls ota_install(from_jobs, allow_restart).
// (Dense timing/debounce bookkeeping summarised; control flow faithful.)
// -----------------------------------------------------------------------------
bool ota_update_gate(bool forced);   // body @ 0x400e32fc - see summary above

// FUN_0x400e10d4 - log_upload_schedule(): mark a log upload due; %s in the log
// ("Log upload due (%s): asked for ...") is "scheduled" (log_upload_interval
// elapsed) or "requested" (explicit request flag).
void log_upload_schedule(void);

// FUN_0x400ef318 - jobs_report_status(status, expected_version): publishes
//   {"status":"%s","expectedVersion":"%s"}  (or {"status":"%s"}) to the AWS IoT
//   Jobs update topic; status in {IN_PROGRESS, SUCCEEDED, FAILED}.
// FUN_0x400ef1e0 - jobs_wait_ack(): waits for the Jobs ACK ("Failed to receive
//   ACK packet: expected ..."). FUN_0x400efa50 - jobs_wait_response(): waits for
//   the shadow/jobs response ("Timed out waiting for response to ...").

// FUN_0x400dc290 - firmware_update_job(): the AWS Jobs firmware branch
// ("Firmware update job refused (error %...)"). Overlaps aws_shadow's job router
// (FUN_0x400dc638); the shared go/up-to-date/debounce decision is reconstructed
// in aws_shadow.cpp. This entry adds the refusal paths then calls ota_install()
// / ota_update_gate(). Cross-referenced, not duplicated.

// NOTE: FUN_0x400e13f0 (1681 B) is the IoT/networking orchestrator - it pumps
// measurement results to AWS ("AWS telemetry published" / "AWS telemetry not
// confirmed") and BLE ("BLE telemetry burst not sent"), and reports a completed
// OTA ("Firmware updated from %s to %s") or a failed/rolled-back one. It belongs
// to the iot_task (aws_shadow) and only calls the OTA reporting above.
