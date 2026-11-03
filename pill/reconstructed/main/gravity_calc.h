/* gravity_calc.h — RAPT Pill 2.0.0, reverse-engineered (readable) reconstruction
 *
 * Provenance: recovered from app_2.0.0.bin (ESP-IDF, ESP32 Xtensa) with Ghidra.
 * NOT original source. Names/types inferred from embedded log & assert strings.
 * See reconstructed/docs/RECONSTRUCTION_NOTES.md. Confidence: Medium-High.
 *
 * This module covers three related concerns that share the "gravity" rodata
 * cluster and the automation state:
 *   1. gravity-from-tilt-angle via a tinyexpr expression (default polynomial),
 *   2. the calibration-run state machine (water/wort/custom points),
 *   3. the automation/alert engine and its persisted event outbox.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- gravity expression ------------------------------------------------- */

/* NVS key "gravity_expr". When unset the firmware compiles this default
 * (observed verbatim in main's init, FUN_0x400df878). Evaluated with tinyexpr
 * over a single variable "angle" (degrees). Result is specific gravity. */
#define GRAVITY_EXPR_DEFAULT \
  "(3.11683E-06 * angle^5 - 0.000786275 * angle^4 + 0.079045 * angle^3 " \
  "- 3.96825 * angle^2 + 100.097 * angle - 1013.07) / 1000 + 1"

/* Valid reported gravity window (DAT_400d09b0 .. DAT_400d09ac). */
#define GRAVITY_MIN 0.900f
#define GRAVITY_MAX 1.200f

/* Compile/evaluate the stored (or default) gravity expression. tinyexpr.
 * Returns specific gravity for a tilt angle in degrees. /* inferred names */ */
bool    gravity_expr_load(void);                 /* FUN_0x400df878 (in main) */
float   gravity_from_angle(float angle_deg);     /* tinyexpr te_eval */
int     gravity_expr_validate(const char *expr); /* FUN_0x400df798: nesting/compile check */

/* ---- calibration run ---------------------------------------------------- */

/* Phase of the live calibration run (serialised as "phase"). FUN_0x400dda60. */
typedef enum {
    CAL_PHASE_IDLE      = 0,   /* "" */
    CAL_PHASE_SETTLING  = 1,   /* "settling" */
    CAL_PHASE_SAMPLING  = 2,   /* "sampling" */
    CAL_PHASE_DONE      = 3,   /* "done"     */
    CAL_PHASE_FAILED    = 4,   /* "failed"   */
    CAL_PHASE_CANCELLED = 5,   /* "cancelled"*/
} cal_phase_t;

/* Failure reason when phase == FAILED (serialised as "error"). FUN_0x400dda94. */
typedef enum {
    CAL_ERR_NONE         = 0,  /* "" */
    CAL_ERR_UNSTABLE     = 1,  /* "unstable"     */
    CAL_ERR_MOVED        = 2,  /* "moved"        */
    CAL_ERR_INCONSISTENT = 3,  /* "inconsistent" */
    CAL_ERR_NO_CURVE     = 4,  /* "no_curve"     */
    CAL_ERR_SENSOR       = 5,  /* "sensor"       */
} cal_error_t;

/* calibration_state values (NVS "calibration_state"), FUN_0x400dac8c parses. */
typedef enum {
    CAL_STATE_NONE      = 0,   /* "none"      */
    CAL_STATE_ONE_POINT = 1,   /* "one_point" */
    CAL_STATE_TWO_POINT = 2,   /* "two_point" */
    CAL_STATE_CUSTOM    = 3,   /* "custom"    */
    CAL_STATE_UNKNOWN   = 4,   /* parse error */
} cal_state_t;

/* Live calibration run (global DAT_400d10dc, int[10] + lock DAT_400d10f0).
 * Field offsets are the decompiled array indices (×4 bytes). */
typedef struct {
    uint32_t id;          /* [0] run counter, bumped each start      */
    uint32_t kind;        /* [1] 0 = "water" point, else "wort"      */
    float    gravity;     /* [2] target gravity (g/L) of this point  */
    cal_phase_t phase;    /* [3] see cal_phase_t                      */
    cal_error_t error;    /* [4] failure reason                      */
    uint32_t t_start;     /* [5] start tick (ms)                     */
    uint32_t t_update;    /* [6] last-update tick (ms)               */
    float    angle;       /* [7] measured tilt angle (settling/samp) */
    float    spread;      /* [8] angle spread during sampling        */
    uint32_t progress;    /* [9] two-point progress counter          */
} cal_run_t;

void cal_run_init(void);                           /* FUN_0x400de370 serialises to JSON */
void cal_run_begin(uint32_t kind, float gravity, uint32_t now); /* FUN_0x400ddac8 */
void cal_run_advance(cal_phase_t phase, uint32_t info);         /* FUN_0x400ddb24 logs "Run %u (%s) %s" */
bool cal_run_cancel(void);                         /* FUN_0x400ddc60 "cancelled before it began" */
void cal_calibration_clear(void);                  /* FUN_0x400ddcd8 takes commit lock, "Calibration cleared" */

/* Validate a desired-state calibration patch before applying. FUN_0x400e5f20.
 * Rejection reasons are the embedded strings, e.g.
 *   "calibration points need calibration_state in the same patch"
 *   "two_point calibration needs distinct stored angles"
 *   "calibration_state custom needs a gravity_expr (none stored)". */
int  calibration_patch_validate(float patch /* JSON obj */, void *out_err, void *ctx);

/* ---- automation / alerts / outbox --------------------------------------- */

/* Alert trigger & operator (parsed in FUN_0x400d9200). */
typedef enum { TRIG_TEMPERATURE = 0, TRIG_GRAVITY = 1, TRIG_GRAVITY_VELOCITY = 2 } alert_trigger_t;
typedef enum { OP_GREATER_THAN = 0, OP_LESS_THAN = 1 } alert_op_t;

/* One alert rule (0x48 bytes in the automation struct). */
typedef struct {
    uint8_t  trigger;     /* +0x00 alert_trigger_t   */
    uint8_t  op;          /* +0x01 alert_op_t        */
    uint8_t  reached;     /* +0x02 fired flag        */
    float    threshold;   /* +0x04                   */
    char     text[0x40];  /* +0x08 "alertText"       */
} alert_rule_t;           /* layout: trigger/op/reached at +0,+1,+2; value +4; text +8 */

/* Persisted automation (global DAT_400d0804, NVS key "automation", 0x710 bytes).
 * +0x04 epoch, +0x0c active, +0x0d n_alerts, +0x0e outbox_count (max 12),
 * +0x08 event_id, +0x10 start_time(i64), alerts[] from +0x84 (0x48 each),
 * outbox[] from +0x2f0 (0x58 each: id, type, ts(i64), text[0x40]@+0x308). */
void automation_start(float obj /*JSON*/, const char *id, const char *name,
                      void *err_cb, char *err_ctx);     /* FUN_0x400d9200 */
void automation_evaluate_reading(float *reading);        /* FUN_0x400d9bc0 */
void automation_stop(const char *why);                   /* FUN_0x400d95f8 "%s stopped" */
void automation_outbox_push(uint8_t type, const char *text); /* FUN_0x400d9124 */
void automation_outbox_ack(int up_to_id);                /* FUN_0x400da09c */
bool automation_status(void *name_out, uint32_t n, int *elapsed_out); /* FUN_0x400da118 */
void automation_persist(void);                           /* FUN_0x400d91d8 */
void automation_job(float job /*JSON*/);                 /* FUN_0x400d979c handles cloud job */

#ifdef __cplusplus
}
#endif
