// aws_shadow.h — RAPT Pill 2.0.0 AWS IoT device-shadow + jobs client
//                (reverse-engineered)
//
// Reverse-engineered from app_2.0.0.bin. NOT original source.
// Tags: "aws", "aws_port", "aws_jobs", "ble_devinfo". Confidence: HIGH for the
// protocol flow (classic AWS IoT Device Shadow + Jobs); MEDIUM for struct types.
//
// The device keeps a mirror of the shadow "reported" state, and when the cloud
// sends a "desired" delta it applies it then echoes the values back into
// "desired" (with a one-shot clientToken) so the service clears the delta.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "json_util.h"

// MQTT topic kinds understood by generate_topic_name(). @ 0x400ef008
// 0..7 are AWS IoT classic Device-Shadow operations assembled with the AWS
// Shadow library (Shadow_AssembleTopicString); 8..10 are app topics built as
// "rapt/<thing>/<suffix>"; 0xb..0xd are AWS IoT Jobs topics.
typedef enum {
    TOPIC_SHADOW_GET            = 0,  // -> ShadowTopicString GetAccepted(1)/... mapping
    TOPIC_SHADOW_GET_ACCEPTED   = 1,
    TOPIC_SHADOW_GET_REJECTED   = 2,
    TOPIC_SHADOW_UPDATE         = 3,  // publish target for reported state
    TOPIC_SHADOW_UPDATE_ACCEPTED= 4,
    TOPIC_SHADOW_UPDATE_REJECTED= 5,
    TOPIC_SHADOW_UPDATE_DELTA   = 6,
    TOPIC_SHADOW_GET2           = 7,  // (also used for /telemetry: "rapt/<thing>/telemetry")
    TOPIC_ALERTS                = 8,  // "rapt/<thing>/alerts"
    TOPIC_HEARTBEAT             = 9,  // "rapt/<thing>/heartbeat"
    TOPIC_LOGS                  = 10, // "rapt/<thing>/logs"
    TOPIC_JOBS_GET_PENDING      = 0xb,
    TOPIC_JOBS_NEXT             = 0xc, // describe "$next"
    TOPIC_JOBS_UPDATE           = 0xd,
} topic_kind_t;

// Source of an incoming shadow document. /* values inferred from call sites */
typedef enum {
    SHADOW_IN_GET_ACCEPTED    = 0,
    SHADOW_IN_UPDATE_ACCEPTED = 4,
    SHADOW_IN_UPDATE_DELTA    = 7,
} shadow_in_t;

// Fill the module topic buffer for `kind`. @ 0x400ef008
void generate_topic_name(topic_kind_t kind);

// Build + publish this device's reported state to $aws/.../shadow/update.
// Also (re)mirrors desired when a mirror is pending, using a clientToken so the
// device can recognise and ignore its own resulting delta. @ 0x400ef3dc
void shadow_publish_reported_state(void);

// Handle a shadow document arriving on get/accepted, update/accepted, or
// update/delta: strip "metadata", parse, drop stale/own updates, apply the
// "desired" patch, persist the shadow version. @ 0x400f07f8
void shadow_handle_incoming(shadow_in_t source, const char *text,
                            const char *desired_path, int notify_on_change);

// Validate then apply a desired patch (also the BLE "desired" write path).
// @ 0x400ede24.  dry_run!=0 validates only.
int  shadow_apply_desired_patch(js_value *patch, int dry_run, char *changed_out);

// Service pending AWS IoT Jobs (firmware-update jobs + calibration jobs).
// @ 0x400dc638
void shadow_process_jobs(void);

// Upload the diagnostic log to rapt/<thing>/logs in parts. @ 0x400dbffc
void shadow_upload_log(const char *buf, int reserve, const uint8_t *end);

// Add calibration_state / calibration_density[] / calibration_angle[] /
// gravity_expr to a JSON object (used when building reported state). @ 0x400daa4c
void shadow_add_calibration(js_value *obj);
