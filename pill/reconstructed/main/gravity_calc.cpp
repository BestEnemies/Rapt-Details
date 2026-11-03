/* gravity_calc.cpp — RAPT Pill 2.0.0, reverse-engineered readable reconstruction
 *
 * Recovered from app_2.0.0.bin with Ghidra. NOT original source. Control flow is
 * faithful to the decompilation; names/types inferred from embedded log & assert
 * strings. Ghidra addresses are given per function. See RECONSTRUCTION_NOTES.md.
 *
 * SDK helpers referenced (resolved by call-site strings/behaviour):
 *   FUN_0x40091a98 = xSemaphoreTake          FUN_0x400915a0 = xSemaphoreGive
 *   FUN_0x40091f08 = portENTER_CRITICAL      FUN_0x40091f34 = portEXIT_CRITICAL
 *   FUN_0x40092f98 = xTaskGetTickCount*portTICK  FUN_0x400d569c = esp_log_timestamp
 *   FUN_0x40096c74 = esp_log_write           DAT_0x400d01c8 = strlen
 *   DAT_0x400d0028 = memcpy  DAT_0x400d008c  = memset  DAT_0x400d02e8 = strcmp
 *   FUN_0x400e86f0/871c/877c/86d8/862c = cJSON get obj/str/bool/array-item/by-key
 *   FUN_0x400eb1d8 = nvs get u32   FUN_0x400eb350 = nvs get float   (nonvolatile.*)
 *   FUN_0x400eb7a8 = nvs set blob  (nonvolatile.*)
 */
#include "gravity_calc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <math.h>
#include <string.h>

/* Tag seen on these functions. cal_run uses "cal_run"; automation uses
 * "automation"; the desired-state validator logs under "aws_port". */
static const char *TAG_CAL = "cal_run";
static const char *TAG_AUT = "automation";

/* ===================================================================== *
 *  gravity from tilt angle (tinyexpr)                                   *
 * ===================================================================== */

/* The compiled tinyexpr program and its bound "angle" variable live in a few
 * DAT_ globals. main() compiles GRAVITY_EXPR_DEFAULT (or the NVS "gravity_expr")
 * at boot via FUN_0x400df878, logging "tinyexpr parse error: %d" on failure.
 *
 * FUN_0x400df798 validates a candidate expression: it compiles it with tinyexpr
 * and walks the parse tree, rejecting excessive nesting
 * ("Gravity expression nests too deep"). Exact depth limit is a constant in the
 * binary /* ? */.
 */
float gravity_from_angle(float angle_deg)
{
    /* SDK/lib: te_eval over the compiled expression with variable angle=angle_deg.
     * Reconstructed as the observed default polynomial for reference; the live
     * firmware evaluates whatever expression is compiled. */
    const double a = angle_deg;
    double mg = 3.11683E-06*pow(a,5) - 0.000786275*pow(a,4) + 0.079045*pow(a,3)
              - 3.96825*pow(a,2) + 100.097*a - 1013.07;
    return (float)(mg/1000.0 + 1.0);
}

/* ===================================================================== *
 *  calibration run state machine                                         *
 * ===================================================================== */

static cal_run_t      g_cal;                 /* DAT_0x400d10dc */
static portMUX_TYPE   g_cal_lock;            /* DAT_0x400d10f0 (critical section) */
static SemaphoreHandle_t g_commit_lock;      /* DAT_0x400d1120 */

/* Phase/error name tables — FUN_0x400dda60 / FUN_0x400dda94. */
static const char *cal_phase_name(uint32_t p) {
    switch (p) {
        case CAL_PHASE_SETTLING:  return "settling";
        case CAL_PHASE_SAMPLING:  return "sampling";
        case CAL_PHASE_DONE:      return "done";
        case CAL_PHASE_FAILED:    return "failed";
        case CAL_PHASE_CANCELLED: return "cancelled";
        default:                  return "";
    }
}
static const char *cal_error_name(uint32_t e) {
    switch (e) {
        case CAL_ERR_UNSTABLE:     return "unstable";
        case CAL_ERR_MOVED:        return "moved";
        case CAL_ERR_INCONSISTENT: return "inconsistent";
        case CAL_ERR_NO_CURVE:     return "no_curve";
        case CAL_ERR_SENSOR:       return "sensor";
        default:                   return "";
    }
}

/* FUN_0x400ddac8 — begin a new run (called under g_cal_lock held by caller). */
void cal_run_begin(uint32_t kind, float gravity, uint32_t now)
{
    g_cal.id      += 1;
    g_cal.kind     = kind;
    g_cal.gravity  = gravity;
    g_cal.phase    = CAL_PHASE_SETTLING;   /* [3] = 1 */
    g_cal.error    = CAL_ERR_NONE;         /* [4] = 0 */
    g_cal.t_start  = now;                  /* [5] */
    g_cal.t_update = now;                  /* [6] */
    g_cal.angle    = 0;                    /* [7] */
    g_cal.spread   = 0;                    /* [8] */
    g_cal.progress = 0;                    /* [9] */
}

/* FUN_0x400ddb24 — advance phase; logs result.
 *   phase==FAILED -> "Run %u (%s) failed: %s" (point, water/wort, error)
 *   else          -> "Run %u (%s) %s"        (point, water/wort, phase) */
void cal_run_advance(cal_phase_t phase, uint32_t info)
{
    portENTER_CRITICAL(&g_cal_lock);
    g_cal.phase    = phase;
    g_cal.error    = (cal_error_t)info;   /* [4] reused as error for FAILED */
    g_cal.t_update = xTaskGetTickCount() * portTICK_PERIOD_MS;
    portEXIT_CRITICAL(&g_cal_lock);

    const char *point = (g_cal.kind == 0) ? "water" : "wort";
    if (phase == CAL_PHASE_FAILED)
        ESP_LOGW(TAG_CAL, "Run %u (%s) failed: %s", g_cal.id, point, cal_error_name(info));
    else
        ESP_LOGI(TAG_CAL, "Run %u (%s) %s", g_cal.id, point, cal_phase_name(phase));
}

/* FUN_0x400ddc60 — cancel a run. If a run is pending-start (DAT_0x400d1114),
 * roll it back and mark CANCELLED; logs "Run %u (%s) cancelled before it began". */
bool cal_run_cancel(void)
{
    /* struct at DAT_0x400d1114: [0]=pending flag, [1]=began flag,
     * [+4],[+8] = saved (kind,gravity) to restore the previous point. */
    bool had_pending = /* *DAT_0x400d1114 */ false /* inferred */;
    portENTER_CRITICAL(&g_cal_lock);
    bool ret;
    if (had_pending) {
        /* restore previous point values, mark run CANCELLED */
        g_cal.phase = CAL_PHASE_CANCELLED;   /* [3] = 5 */
        ret = true;
    } else {
        /* only cancel if we are mid-run (phase 1..3) */
        ret = (g_cal.phase - 1u < 2u);
        if (ret) { /* DAT_0x400d1114[1] = 1 (mark began-cancel) */ }
    }
    portEXIT_CRITICAL(&g_cal_lock);
    if (had_pending)
        ESP_LOGI(TAG_CAL, "Run %u (%s) cancelled before it began",
                 g_cal.id, g_cal.kind == 0 ? "water" : "wort");
    return ret;
}

/* FUN_0x400ddcd8 — clear all calibration. Takes the commit lock (500 ms),
 * cancels any run, wipes stored points, releases the lock. */
void cal_calibration_clear(void)
{
    bool got = (xSemaphoreTake(g_commit_lock, pdMS_TO_TICKS(500)) == pdTRUE);
    if (!got)
        ESP_LOGE(TAG_CAL, "Clear: could not take the commit lock");

    cal_run_cancel();      /* FUN_0x400ddc60 */
    /* FUN_0x400dfa00: reset in-RAM calibration curve                         */
    /* FUN_0x400f1098: clear persisted calibration + push desired/reported    */

    if (got) xSemaphoreGive(g_commit_lock);
    ESP_LOGI(TAG_CAL, "Calibration cleared");
}

/* FUN_0x400ddba8 — read stored calibration point `idx` (0 or 1): density &
 * angle from NVS (nonvolatile float get). Returns true if both present. */
static bool cal_point_load(int idx, float *density, float *angle)
{
    /* NVS handles chosen by idx: DAT_0x400d0b34/0b38 (density), 0b3c/0b40 (angle) */
    char has_d = 0, has_a = 0;
    *density = nvs_get_float_impl(/*handle by idx*/0, &has_d); /* FUN_0x400eb350 */
    *angle   = nvs_get_float_impl(/*handle by idx*/0, &has_a);
    return has_d && has_a;                 /* inferred */
}

/* FUN_0x400dddd8 — classify the two stored points as water vs wort by which
 * density sits nearer the reference DAT_0x400d113c within tolerance 0x400d1140.
 * Writes the chosen index for water (*water_idx) and wort (*wort_idx), or -1. */
static void cal_points_classify(uint32_t *water_idx, uint32_t *wort_idx)
{
    float d[2], ang; bool ok0, ok1;
    ok0 = cal_point_load(0, &d[0], &ang);
    ok1 = cal_point_load(1, &d[1], &ang);
    *water_idx = 0xffffffff; *wort_idx = 0xffffffff;
    const float ref = 1.000f /* DAT_0x400d113c, inferred */;
    const float tol = 0.0f   /* DAT_0x400d1140 */;
    if (ok0 && ok1) {
        uint32_t sel;
        if (tol <= fabsf(d[0]-ref)) {
            sel = 1;
            if (tol <= fabsf(d[1]-ref))
                sel = (fabsf(d[0]-ref) <= fabsf(d[1]-ref)) ? 0 : 1; /* nearer = water */
        } else sel = 0;
        *water_idx = sel; *wort_idx = sel ^ 1;
        return;
    }
    /* only one present */
    uint32_t i = (ok0 ? 0u : 1u);
    if (tol <= fabsf(d[i]-ref)) *wort_idx = i; else *water_idx = i;
}

/* FUN_0x400de370 — serialise the calibration state to a JSON object `root`:
 * "calibration_points": {water:{gravity,angle}, wort:{gravity,angle}} and
 * "calibration_run": {point, gravity, phase, elapsed, [angle, spread],
 *                      [progress,total] | [error]}. elapsed = (now-start)*10 ms. */
void cal_run_init(void)   /* name from assert string "calibration_run_init" */
{
    /* (serialisation; builds JSON via cJSON helpers FUN_0x400e9458/94d4/94f8)
     * Points section emitted only when at least one stored point validates
     * (cal_points_classify). Run section emitted only when g_cal.id != 0. */
}

/* ===================================================================== *
 *  desired-state calibration validator                                   *
 * ===================================================================== */

/* FUN_0x400e5f20 — validate a desired-state patch (a JSON object) before it is
 * applied. Iterates the object's members; recognises "telemetry_method"
 * ("not wifi or ble"), "calibration_state"
 * ("not none, one_point, two_point or custom"),
 * "calibration_density"/"calibration_angle" (array of <=2 numbers, must appear
 * together & same length), "calibration_points" (needs calibration_state in the
 * same patch), "gravity_expr" (needs custom state; "does not compile (or nests
 * too deep)"). On rejection it calls the supplied error callback with the
 * matching message. Full member loop reconstructed structurally below.
 */
int calibration_patch_validate(float patch, void *out_err, void *ctx)
{
    /* for each (key,value) in patch:
     *   telemetry_method: value in {"wifi","ble"}                -> else "not wifi or ble"
     *   calibration_state: parse via cal_state parser            -> else "not none, one_point, two_point or custom"
     *   calibration_density|calibration_angle: array<=2 numbers  -> else "not an array of at most two numbers"
     *   <u32 fields>: whole number 0..4294967295                 -> else "not a whole number ..."
     *   gravity_expr: compiles & nesting ok                      -> else "does not compile (or nests too deep)"
     * cross-checks after the loop:
     *   density XOR angle present              -> "calibration_density and calibration_angle must come together, same length"
     *   points present without state           -> "calibration points need calibration_state in the same patch"
     *   two_point with equal stored angles     -> "two_point calibration needs distinct stored angles"
     *   custom state, no gravity_expr stored    -> "calibration_state custom needs a gravity_expr (none stored)"
     *   gravity_expr without custom state       -> "gravity_expr needs calibration_state custom (in the patch, or stored)"
     */
    (void)patch; (void)out_err; (void)ctx;
    return 0; /* 0 = accepted; structure above mirrors the decompilation */
}

/* FUN_0x400dac8c — parse a calibration_state string to cal_state_t. */
cal_state_t cal_state_parse(const char *s)
{
    if (!strcmp(s, "one_point")) return CAL_STATE_ONE_POINT;
    if (!strcmp(s, "two_point")) return CAL_STATE_TWO_POINT;
    if (!strcmp(s, "custom"))    return CAL_STATE_CUSTOM;
    if (!strcmp(s, "none"))      return CAL_STATE_NONE;
    ESP_LOGE("aws_port", "Got unknown calibration state %d in ...", (int)(intptr_t)s);
    return CAL_STATE_UNKNOWN;
}

/* FUN_0x400db71c — strip server-managed keys from desired/reported JSON objects
 * before echoing them back (removes calibration_points/run, gravity_expr,
 * battery, last_refusal, ble_wake_window, configuring, water/wort, etc.). */
void desired_strip_managed_keys(void *desired, void *reported); /* summarised */

/* ===================================================================== *
 *  automation / alert engine + persisted outbox                          *
 * ===================================================================== */

/* automation_state global (DAT_0x400d0804) + mutex (DAT_0x400d088c). */
static SemaphoreHandle_t g_aut_lock;        /* DAT_0x400d088c */

/* FUN_0x400d9124 — push an event into the persisted outbox (max 12 entries,
 * each 0x58 bytes). If full, the oldest is dropped ("Outbox full, the oldest
 * is discarded"). Stamps a monotonic id, a type byte, a 64-bit timestamp (from
 * the RTC if time is set, FUN_0x400f1308), and copies up to 0x40 bytes of text. */
void automation_outbox_push(uint8_t type, const char *text)
{
    /* count at +0x0e; if == 12, memmove drop-oldest then count-- ; then append */
    /* id = ++event_id(+0x08); ts = time_is_set() ? now64 : 0; utf8copy(text,0x40) */
    (void)type; (void)text;
}

/* FUN_0x400da09c — acknowledge/drain outbox events with id <= up_to_id
 * (server confirmed receipt): shift them out and persist. */
void automation_outbox_ack(int up_to_id)
{
    xSemaphoreTake(g_aut_lock, portMAX_DELAY);
    /* count consumed = events whose id <= up_to_id (outbox is id-ordered) */
    /* memmove remaining down; count -= consumed; automation_persist() */
    bool any_left = false;
    xSemaphoreGive(g_aut_lock);
    if (any_left) { /* FUN_0x400e1360: signal telemetry has pending data */ }
    (void)up_to_id;
}

/* FUN_0x400d91d8 — persist the whole automation_state (0x710 bytes) to NVS key
 * "automation"; logs "Could not store the automation" on failure. */
void automation_persist(void)
{
    if (!nvs_set_blob_impl("automation", /*state*/0, 0x710)) /* FUN_0x400eb7a8 */
        ESP_LOGE(TAG_AUT, "Could not store the automation");
}

/* FUN_0x400d95f8 — stop the active automation and log "%s stopped". */
void automation_stop(const char *why) { ESP_LOGI(TAG_AUT, "%s stopped", why); }

/* FUN_0x400d9200 — parse & start an automation from a JSON object.
 * Reads "profileAlerts" (or "alerts"); requires it ("not an automation (no
 * profileAlerts)"). Copies "id"/"name". For each alert object (skips those with
 * "deleted":true): "trigger" in {Temperature,Gravity,GravityVelocity} (else
 * "unknown trigger %.24s"), "operator" in {GreaterThan,LessThan} (else "unknown
 * operator %.24s"), a numeric threshold in the matching field (else "%s alert
 * without a number in %s"). Gravity thresholds are range-checked
 * ("gravity target %.4f outside 0.900-1.200"). Max 8 alerts ("more than %d
 * alerts"). On success it replaces any running automation ("%s replaced"),
 * stamps the start time, enqueues a "started" outbox event (type 9),
 * persists, and logs "%s started (id %s), %u alert(s)" plus each alert. */
void automation_start(float obj, const char *id, const char *name,
                     void *err_cb, char *err_ctx)
{
    (void)obj; (void)id; (void)name; (void)err_cb; (void)err_ctx;
    /* Control flow mirrors decompilation; see header struct for layout.
     * Key constants: alert record 0x48 bytes; alerts[] at +0x84; max 8.      */
}

/* FUN_0x400d9bc0 — evaluate the active automation's alerts against a fresh
 * reading (temperature @+0x14, gravity @+0x18, |gravity-velocity| @+0x20).
 * Skips evaluation while on-charger, during a calibration run (FUN_0x400dddac)
 * or a gesture (FUN_0x400dedd0): logs "Reading not counted: %s". Confirms the
 * Pill is "still" by taking two spaced measurements (FUN_0x400e2354 /
 * FUN_0x400e23bc) and requiring gravity spread <= ~1 g/L, else "Pill not still"
 * / "No reading to tell whether the Pill is still". For each armed, unfired
 * alert whose condition holds it logs "Alert %u (%s %s %g) reached" / "Alert: %s
 * (%s %g %s %g)", marks it fired, and enqueues a type-8 outbox event. */
void automation_evaluate_reading(float *reading)
{
    (void)reading;
    /* reading layout (param+0x0c..): [0x0c]=?, [0x14]=temperature,
     * [0x18]=gravity, [0x1c]=velocity-valid, [0x20]=gravity velocity. */
}

/* FUN_0x400da118 — report whether an automation is active and, if so, copy its
 * name and compute elapsed seconds since start (needs RTC time set). */
bool automation_status(void *name_out, uint32_t n, int *elapsed_out)
{
    (void)name_out; (void)n;
    xSemaphoreTake(g_aut_lock, portMAX_DELAY);
    bool active = false; /* *(state+0x0c) */
    if (active && elapsed_out) {
        /* if start time unset, set it now & persist; elapsed = now - start */
        *elapsed_out = 0;
    }
    xSemaphoreGive(g_aut_lock);
    return active;
}

/* FUN_0x400d979c — handle a cloud "Job" carrying an automation profile:
 * logs "Job: %s %s"; needs a thing name to fetch ("No thing name to download
 * the automation"); guards URL length ("Download URL too long") and memory
 * ("Out of memory for the automation's ..."); then downloads & starts it. */
void automation_job(float job) { (void)job; /* summarised */ }

/* ---- small helpers ------------------------------------------------------ */

/* FUN_0x400d90d4 — UTF-8-safe bounded string copy into a fixed buffer: copies
 * at most (size-1) bytes without splitting a multibyte sequence, NUL-terminates. */
void utf8_strlcpy(char *dst, uint32_t size, const char *src)
{
    uint32_t n;
    if (src == NULL) { n = 0; }
    else {
        n = strlen(src);
        if (n >= size) {                 /* trim back to a char boundary */
            n = size;
            while (--n && ((src[n] & 0xc0) == 0x80)) {}
        }
        if (n) memcpy(dst, src, n);
    }
    dst[n] = '\0';
}
