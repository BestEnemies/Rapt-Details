// aws_shadow.cpp — RAPT Pill 2.0.0 AWS IoT device-shadow + jobs client
//                  (reverse-engineered)
//
// Reverse-engineered from app_2.0.0.bin. NOT original source.
// Confidence: HIGH for protocol flow; struct offsets/globals marked /* inferred */.
//
// Globals in the decompilation (RAM, with my reconstructed names):
//   DAT_400d2854  thing_name (char*)          DAT_400d2864  topic_buf[0x100]
//   DAT_400d2830  have_shadow_version (bool)  DAT_400d28e8  shadow_version (u32)
//   DAT_400d2834  expect_own_delta (bool)     DAT_400d2904  own_update_version (u32)
//   DAT_400d2804  mirror_pending (bool)       DAT_400d2808  mirror_requested (bool)
//   DAT_400d28e4  mirror_token[0x18]          DAT_400d28dc  token_prefix
//   DAT_400d28d8  token_counter (u32)
//   DAT_400d28a0  mqtt_context                mirror_mutex (FUN_400eede0/dc8 lock/unlock)
//
// The memw()/watchdog critical-section artifacts in the decompilation are FreeRTOS
// scheduler bookkeeping and are omitted here for readability.

#include "aws_shadow.h"
#include "ota.h"
#include "json_util.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

// From other modules (reconstructed elsewhere):
extern const char *g_thing_name;                 // DAT_400d2854
extern char        g_topic_buf[0x100];            // DAT_400d2864
// nonvolatile.cpp:
int  nonvolatile_set_u32(void *handle, uint32_t v);   // FUN_400eb290
int  nonvolatile_get_str(void *handle, char *out, int cap); // FUN_400eb508
// measurement.cpp / gravity_calc.cpp add their fields to a JSON object:
void measurement_add_reported(js_value *reported);        // FUN_400dad74
void measurement_add_desired_mirror(js_value *desired);   // FUN_400db7b0
void gravity_add_calibration(js_value *obj, js_value *desired); // FUN_400db71c
void gravity_apply_desired(js_value *desired, int apply, char *changed); // FUN_400db41c
void shadow_note_report_needed(void);             // FUN_400dbfdc / FUN_400f07c8

static const char *TAG      = "aws";       // PTR_s_aws_3f407836
static const char *TAG_PORT = "aws_port";
static const char *TAG_JOBS = "aws_jobs";

// mirror-token mutex (FUN_400eede0 lock / FUN_400eedc8 unlock) — a FreeRTOS mutex.
static void mirror_lock(void);
static void mirror_unlock(void);

// ---- AWS IoT library bindings (names recovered from asserts "js == JobsSuccess") ----
//   FUN_401045c0 = Shadow_AssembleTopicString   FUN_40104868/40104a9c = Jobs_* builders
//   FUN_40102d30 = MQTT process loop            FUN_400ef278 = mqtt publish(topic, json)
extern "C" {
int Shadow_AssembleTopicString(int type, const char *thing, uint8_t thinglen,
                               const char *shadow, int shadowlen, char *buf,
                               uint16_t buflen, uint16_t *outlen);     // FUN_401045c0
int Jobs_GetPending(char *buf, int cap, const char *thing, uint16_t tl,
                    int api, uint16_t *outlen);                        // FUN_40104868 (api=1)
int Jobs_Update(char *buf, int cap, const char *thing, uint16_t tl,
                int api, uint16_t *outlen);                            // FUN_40104868 (api=6)
int Jobs_Describe(char *buf, int cap, const char *thing, uint16_t tl,
                  const char *jobid, int api, uint16_t *outlen);       // FUN_40104a9c ("$next")
int mqtt_publish(const char *topic, const char *payload);              // FUN_400ef278
int mqtt_process_loop(void *ctx);                                      // FUN_40102d30
}

// ---------------------------------------------------------------------------
// @ 0x400ef008 — build an MQTT topic into g_topic_buf (0x100 bytes).
// ---------------------------------------------------------------------------
void generate_topic_name(topic_kind_t kind)
{
    const char *thing = g_thing_name;
    uint16_t outlen = 0;

    // Map our kind to AWS Shadow ShadowTopicStringType for the 0..7 range.
    //   1->GetAccepted(1) 2->GetRejected(2) 3->Update(6) 5->UpdateRejected(8)
    //   6->UpdateDelta(10) 0/4/7->UpdateAccepted(7)
    auto shadow_type = [](topic_kind_t k) -> int {
        switch (k) {
            case 1: return 1; case 2: return 2; case 3: return 6;
            case 5: return 8; case 6: return 10; default: return 7;
        }
    };

    switch (kind) {
        case TOPIC_SHADOW_GET2: // 7 also doubles as the telemetry topic
            snprintf(g_topic_buf, 0x100, "%s%s%s", "rapt/", thing, "/telemetry");
            return;

        case TOPIC_ALERTS:
            snprintf(g_topic_buf, 0x100, "%s%s%s", "rapt/", thing, "/alerts");    return;
        case TOPIC_HEARTBEAT:
            snprintf(g_topic_buf, 0x100, "%s%s%s", "rapt/", thing, "/heartbeat"); return;
        case TOPIC_LOGS:
            snprintf(g_topic_buf, 0x100, "%s%s%s", "rapt/", thing, "/logs");      return;

        case TOPIC_JOBS_GET_PENDING:
            if (Jobs_GetPending(g_topic_buf, 0x100, thing, strlen(thing), 1, &outlen) == 1) return;
            break;
        case TOPIC_JOBS_NEXT:
            if (Jobs_Describe(g_topic_buf, 0x100, thing, strlen(thing), "$next", 5, &outlen) == 1) return;
            break;
        case TOPIC_JOBS_UPDATE:
            if (Jobs_Update(g_topic_buf, 0x100, thing, strlen(thing), 6, &outlen) == 1) return;
            break;

        default: // 0..6 shadow operations
            if (Shadow_AssembleTopicString(shadow_type(kind), thing, strlen(thing),
                                           nullptr, 0, g_topic_buf, 0xff, &outlen) == 0) {
                g_topic_buf[outlen] = '\0';
                return;
            }
            break;
    }
    // On failure the firmware trips an assert ("js == JobsSuccess" / "r == SHADOW_SUCCESS").
    // configASSERT(false);  // FUN_40098260(...)
}

// ---------------------------------------------------------------------------
// @ 0x400ef3dc — build and publish this device's reported state.
// ---------------------------------------------------------------------------
void shadow_publish_reported_state(void)
{
    // Decide (under lock) whether this round also mirrors desired. A desired
    // mirror is requested (g_mirror_requested) when we have applied a delta and
    // want the service to clear it; we emit one new clientToken for it.
    extern bool g_mirror_requested, g_mirror_pending;     // DAT_400d2808 / DAT_400d2804
    extern char g_mirror_token[0x18];                     // DAT_400d28e4
    extern const char *g_token_prefix;                    // DAT_400d28dc
    extern uint32_t g_token_counter;                      // DAT_400d28d8
    extern bool g_have_shadow_version;                    // DAT_400d2830
    extern uint32_t g_shadow_version;                     // DAT_400d28e8
    extern bool g_expect_own_delta;                       // DAT_400d2834
    extern uint32_t g_own_update_version;                 // DAT_400d2904
    extern void *g_shadow_nvs;                            // PTR_DAT_400d282c

    bool mirroring = false;
    mirror_lock();
    if (g_mirror_requested && !g_mirror_pending) {
        g_mirror_requested = false;
        g_mirror_pending   = true;
        g_token_counter++;
        snprintf(g_mirror_token, sizeof(g_mirror_token), "%s%08lx",
                 g_token_prefix, (unsigned long)g_token_counter);
        mirroring = true;
    }
    mirror_unlock();

    js_value *root     = js_new_object();
    js_value *reported = js_new_object();
    if (!root || !reported) {
        ESP_LOGE(TAG, "Could not create reported state JSON");
        if (root) js_free(root);
        if (reported) js_free(reported);
        return;
    }

    bool ok = true;
    // optional top-level "version" for a conditional shadow update
    if (g_have_shadow_version)
        ok = (js_add_i64(root, "version", (int64_t)g_shadow_version) == 0);

    // Fill reported from the measurement + gravity modules.
    measurement_add_reported(reported);          // FUN_400dad74
    gravity_add_calibration(reported, nullptr);  // FUN_400db71c(reported, 0)
    bool nested_ok = (js_add_object(root, "state.reported", reported) == 0);

    char *text = nullptr;
    if (mirroring) {
        // Echo the applied desired values back so the delta is cleared.
        js_value *desired = js_new_object();
        if (desired) {
            measurement_add_desired_mirror(desired);  // FUN_400db7b0
            gravity_add_calibration(nullptr, desired); // FUN_400db71c(0, desired)
            if (js_add_object(root, "state.desired", desired) == 0 &&
                js_add_string(root, "clientToken", g_mirror_token) == 0) {
                if (ok && nested_ok) text = js_serialise(root);
            } else {
                js_free(desired);
                ESP_LOGE(TAG, "Could not add the desired mirror: deferred to the next update");
            }
        }
    } else if (ok && nested_ok) {
        text = js_serialise(root);
    }
    js_free(root);

    if (!text) {
        ESP_LOGE(TAG, "Could not serialise reported state");
        if (mirroring) mirror_pending_clear(/*success=*/false); // FUN_400eedf4(0)
        return;
    }

    ESP_LOGI(TAG, "Reported state: %s", text);
    if (g_have_shadow_version) {
        g_expect_own_delta   = true;
        g_own_update_version = g_shadow_version + 1;  // ignore the delta our own update makes
    }

    generate_topic_name(TOPIC_SHADOW_UPDATE);         // "3"
    bool published = mqtt_publish(g_topic_buf, text);
    js_free_serialised(text);                         // FUN_400e93c4

    if (!published) {
        ESP_LOGE(TAG, "Could not publish to ...");
        if (mirroring) mirror_pending_clear(false);
        return;
    }

    if (mirroring) {
        // Pump MQTT (up to ~1 s) until the mirror update is accepted, so the
        // desired delta is gone before we sleep.
        uint32_t start = now_ms();                    // FUN_40092f98
        while (g_mirror_pending) {
            if (!mirror_still_pending() ||            // FUN_400dbf9c
                (mqtt_process_loop(g_mqtt_context) != 0 /*OK*/ &&
                 mqtt_process_loop(g_mqtt_context) != 0xb) ||
                (now_ms() - start > 999)) {
                mirror_pending_clear(false);          // give up
                return;
            }
            vTaskDelay(pdMS_TO_TICKS(5));             // FUN_40093438(5)
        }
    }
}

// ---------------------------------------------------------------------------
// @ 0x400f07f8 — handle an incoming shadow document.
// ---------------------------------------------------------------------------
void shadow_handle_incoming(shadow_in_t source, const char *text,
                            const char *desired_path, int notify_on_change)
{
    char changed = '\0';

    // 1) In-place strip of the (potentially huge) "metadata" object to save heap
    //    before parsing. The firmware brace-matches "metadata":{...} and removes
    //    it, then logs how many bytes were dropped. Reconstructed intent only:
    int dropped = strip_metadata_inplace((char *)text);  // @ 0x400f07f8 prologue
    if (dropped > 0)
        ESP_LOGD(TAG, "Shadow document: %u bytes of metadata dropped before parsing", dropped);

    js_value *parsed = js_parse(text);
    if (!parsed || js_type(js_root(parsed)) != JS_OBJECT) {
        ESP_LOGE(TAG, "Could not parse the shadow document (%u bytes, free heap %u)",
                 (unsigned)strlen(text), (unsigned)esp_get_free_heap_size());
        // If this is our own mirror update coming back (token present in text),
        // treat the mirror as applied.
        if (source == SHADOW_IN_UPDATE_ACCEPTED && g_mirror_pending &&
            strstr(text, g_mirror_token)) {
            ESP_LOGW(TAG, "Our mirror token is in the text: the update is taken as applied");
            mirror_pending_clear(true);               // FUN_400eedf4(1)
        }
        if (parsed) js_free(parsed);
        return;
    }

    js_value *obj = js_root(parsed);

    // clientToken check (only on update/accepted): is this our own echo?
    bool own_update = false;
    const char *tok = js_get_string(obj, "clientToken");
    if (source == SHADOW_IN_UPDATE_ACCEPTED && tok) {
        if (strlen(tok) == 0x11 && strncmp(tok, g_token_prefix, 9) == 0) {
            own_update = true;
            if (g_mirror_pending && strcmp(tok, g_mirror_token) == 0)
                mirror_pending_clear(true);
        }
    }

    // "version" for staleness / own-delta detection
    bool have_ver = false; uint32_t ver = 0;
    js_value *vmember = js_get_member(obj, "version");
    if (vmember && js_type(vmember) == JS_NUMBER) {
        ver = (uint32_t)js_number(vmember);
        have_ver = true;
    }

    if (source == SHADOW_IN_UPDATE_DELTA) {
        if (g_expect_own_delta && have_ver && ver == g_own_update_version) {
            ESP_LOGI(TAG, "Ignoring /update/delta from our own update");
            g_expect_own_delta = false;
            goto done;
        }
    } else {
        if (g_have_shadow_version && have_ver && ver <= g_shadow_version) {
            ESP_LOGI(TAG, "Ignoring incoming shadow document because it's stale");
            goto done;
        }
    }

    {
        js_value *desired = js_get_member(obj, desired_path); // "state.desired" / "desired"
        if (!desired) {
            if (source == SHADOW_IN_UPDATE_ACCEPTED)
                ESP_LOGD(TAG, "No %s in the accepted update: a reported-only update", desired_path);
            else
                ESP_LOGW(TAG, "Could not get %s from incoming shadow document", desired_path);
        } else if (!own_update && !g_mirror_pending) {
            // Don't clobber a mirror we are about to send on a fresh /get.
            bool skip = false;
            if (source == SHADOW_IN_GET_ACCEPTED) {
                mirror_lock(); skip = g_mirror_requested; mirror_unlock();
            }
            if (!skip)
                gravity_apply_desired(desired, 0, &changed); // FUN_400db41c(desired, 0, &changed)
        }
        // Persist the new shadow version.
        if (have_ver) {
            g_shadow_version = ver;
            g_have_shadow_version = true;
            nonvolatile_set_u32(g_shadow_nvs, ver);   // FUN_400eb290
        }
    }

done:
    js_free(parsed);
    if (changed && notify_on_change) {
        shadow_note_changed();      // FUN_400dbfdc
        shadow_trigger_report();    // FUN_400f07c8
    }
}

// ---------------------------------------------------------------------------
// @ 0x400ede24 — validate (and optionally apply) a desired patch. Also the
// BLE "desired" write entry point. Returns an app status code
// (0 ok/empty, 6 no-settable-fields, 0x13 rejected, 0xe apply-failed).
// ---------------------------------------------------------------------------
int shadow_apply_desired_patch(js_value *patch, int /*dry_run*/ unused, char *changed_out)
{
    int count = 0;
    const settable_field_t *fields = nonvolatile_settable_fields(&count); // FUN_400daf9c
    char errbuf[0x60] = {0};

    int nmembers = js_object_size(patch);             // PTR_FUN_400d1d6c

    if (!fields || count == 0) {
        ESP_LOGW(TAG, "Desired write: this device publishes no settable fields");
        return 6;
    }

    // 1) Per-key type/length validation against the settable-field table.
    bool rejected = false;
    for (int i = 0; i < nmembers; i++) {
        const char *key = js_member_key(patch, i);    // PTR_FUN_400d1d70
        js_value   *val = js_member_val(patch, i);    // PTR_FUN_400d1d74
        int vtype = js_type(val);

        const settable_field_t *f = nullptr;
        for (int j = 0; j < count; j++)
            if (strcmp(fields[j].name, key) == 0) { f = &fields[j]; break; }

        if (!f) {
            snprintf(errbuf, sizeof(errbuf), "unknown key \"%s\"", key);
        } else if (f->type != vtype) {
            snprintf(errbuf, sizeof(errbuf), "\"%s\" is a %s, expected a %s",
                     key, js_type_name(vtype), js_type_name(f->type));
        } else if (vtype == JS_STRING && (unsigned)strlen(js_string(val)) > f->maxlen) {
            snprintf(errbuf, sizeof(errbuf), "\"%s\" is longer than %u bytes", key, f->maxlen);
        } else {
            continue; // this key is fine
        }
        ESP_LOGW(TAG_devinfo, "Desired patch: %s: patch rejected", errbuf);
        rejected = true;
    }
    if (rejected) { nonvolatile_store_refusal(errbuf); return 0x13; } // FUN_400dad3c

    // 2) Semantic validation (calibration consistency etc.).
    if (!gravity_validate_desired(patch, errbuf, sizeof(errbuf))) {    // FUN_400e5f20
        if (!errbuf[0]) strcpy(errbuf, "refused by the device");
        ESP_LOGW(TAG_devinfo, "Desired patch: %s: patch rejected", errbuf);
        nonvolatile_store_refusal(errbuf);
        return 0x13;
    }

    // 3) Apply.
    if (nmembers == 0) {
        ESP_LOGI(TAG_devinfo, "Desired patch is empty");
        return 0;
    }
    ESP_LOGI(TAG_devinfo, "Applying a desired patch: ...");
    char changed = '\0';
    if (gravity_apply_desired(patch, 1, &changed), changed_out ? (*changed_out = changed) : 0, changed) {
        shadow_note_changed();                        // FUN_400dbfdc
        shadow_reschedule();                          // FUN_400f1098
        ESP_LOGI(TAG_devinfo, "Desired patch applied: %s", changed ? "settings changed" : "no change");
        return 0;
    }
    ESP_LOGW(TAG_devinfo, "Validated desired patch ... could not be applied");
    nonvolatile_store_refusal("the patch could not be applied");
    return 0xe;
}

// ---------------------------------------------------------------------------
// @ 0x400dc638 — service pending AWS IoT Jobs.
// ---------------------------------------------------------------------------
void shadow_process_jobs(void)
{
    // (a) firmware-update job already in flight: once its reported status has
    //     changed or ~12 s have passed, log and clear the "in flight" marker.
    if (fw_job_in_flight() &&
        (fw_job_status() != fw_job_last_status || (now_ms() - fw_job_t0) > 0x2edf)) {
        ESP_LOGI(TAG_JOBS, "Firmware update job: going ahead");
        fw_job_in_flight_clear();
    }

    // (b) a newly-arrived firmware-update job.
    if (fw_job_new() && !fw_job_in_flight() && fw_job_read(/*&job*/) == 0) {
        fw_job_new_clear();
        firmware_job_t job; fw_job_parse(&job);      // FUN_400e3994
        if (job.id == fw_job_expected_id) {
            if (job.state == 3 /* target already running */) {
                ESP_LOGI(TAG_JOBS, "Firmware update job: %s is up to date", fw_job_target());
                jobs_report_status(4 /*SUCCEEDED*/, fw_job_target()); // FUN_400dc20c
                shadow_trigger();                                     // FUN_400e1360
            } else if (job.state == 6 && job.go /* queued */) {
                ota_start_from_job(1, /*cfg*/ nullptr);               // FUN_400e3910
            }
        }
    }

    // (c) a calibration job.
    if (cal_job_new() && cal_job_read(/*&res*/) &&
        cal_job_id != cal_job_last_id && cal_job_present) {
        cal_job_new_clear();
        if (cal_job_ok) {
            ESP_LOGI(TAG_JOBS, "Calibration job done ...");
            jobs_report_status(6 /*SUCCEEDED*/, cal_job_result()); // FUN_400dc190(6,...)
        } else {
            ESP_LOGW(TAG_JOBS, "Calibration job failed: %s", cal_job_error_text());
            // map the error text to one of 6 known codes (table @ 0x400d0e14)
            char code = cal_job_map_error(cal_job_error_text());
            jobs_report_status(7 /*FAILED*/, code);                // FUN_400dc190(7,...)
        }
        shadow_trigger();                                          // FUN_400e1360
    }
}

// ---------------------------------------------------------------------------
// @ 0x400dbffc — upload the diagnostic log to rapt/<thing>/logs, in parts.
// Payload per part:
//   {"device_id":"%s","firmware":"%s",[<"timestamp":"...",>]"upload":%lu,
//    "part":%u,"parts":...,[<"truncated":true,>]...}
// ---------------------------------------------------------------------------
void shadow_upload_log(const char *buf, int reserve, const uint8_t *end)
{
    // The firmware walks the log in <=0x103-byte chunks (FUN_400e0e70), builds a
    // JSON envelope per part with an optional "timestamp" and "truncated":true,
    // and publishes it. The first part logs:
    //   "Log upload %lu: %u part(s), %s%s"  (the last %s is ", truncated" if set)
    // Reconstructed intent; byte-level chunking omitted for readability.
    (void)buf; (void)reserve; (void)end;
    // for each part: snprintf(part, 0x100,
    //   "{\"device_id\":\"%s\",\"firmware\":\"%s\",%s\"upload\":%lu,\"part\":%u,\"parts\":...}",
    //   device_id, firmware, truncated ? "\"truncated\":true," : "", upload_id, part_no);
    // mqtt_publish(topic=rapt/<thing>/logs, part);
}

// ---------------------------------------------------------------------------
// @ 0x400daa4c — append calibration fields to a JSON object.
// ---------------------------------------------------------------------------
void shadow_add_calibration(js_value *obj)
{
    // calibration_state: enum from NVS u8 -> "one_point"/"two_point"/"custom"/
    //                    "" (code 3) / "unknown" (default, logs an error).
    uint32_t st = nonvolatile_get_u32(/*cal_state key*/ nullptr, 0); // FUN_400eb1d8
    const char *state;
    switch (st) {
        case 0:  state = "" /* none */; break;   // PTR_DAT_400d0b1c
        case 1:  state = "one_point";   break;
        case 2:  state = "two_point";   break;
        case 3:  state = "custom";      break;
        default:
            ESP_LOGE(TAG_PORT, "Got unknown calibration state %d", (int)st);
            state = "unknown"; break;
    }
    js_add_string(obj, "calibration_state", state);

    // calibration_density[] and calibration_angle[] — number arrays read from
    // blob NVS keys (up to 2 points each); FUN_400eb350 loads the blob, the
    // present flag at [0xfb] gates each push.
    js_value *dens  = js_array_in(obj, "calibration_density");
    js_value *angle = js_array_in(obj, "calibration_angle");
    // (push recovered points; omitted for brevity)
    (void)dens; (void)angle;

    // gravity_expr string (tinyexpr formula) if stored.
    char expr[0x100];
    if (nonvolatile_get_str(/*gravity_expr key*/ nullptr, expr, sizeof(expr)))
        js_add_string(obj, "gravity_expr", expr);
}

// ---- small helpers whose exact bodies live in other modules / SDK ----
// FUN_400eede0/FUN_400eedc8 mirror mutex; FUN_400eedf4 clear mirror pending.
static void mirror_lock(void)   { /* xSemaphoreTake(mirror_mutex) */ }
static void mirror_unlock(void) { /* xSemaphoreGive(mirror_mutex) */ }
