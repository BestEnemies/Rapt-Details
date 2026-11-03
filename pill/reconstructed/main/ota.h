// ota.h — RAPT Pill 2.0.0 firmware OTA update check (reverse-engineered)
//
// Reverse-engineered from app_2.0.0.bin (ESP-IDF, Xtensa). NOT original source.
// See reconstructed/docs/RECONSTRUCTION_NOTES.md. Confidence: HIGH.
//
// The Pill checks for firmware updates against an AWS API Gateway endpoint
// (and, historically, api.rapt.io). The check returns metadata only; the actual
// image download + flash is driven elsewhere (power_management / aws jobs).
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Result of an update check. Non-zero == no usable update this round.
//   0 = ok (parsed; see .update_available)   -> "ok"
//   1 = no response (oom / init / open / read failure) -> "no response"
//   2 = HTTP error (non-200 status)          -> "HTTP error"
//   3 = bad response (not JSON / missing fields / unusable checksum) -> "bad response"
typedef enum {
    OTA_CHECK_OK          = 0,
    OTA_CHECK_NO_RESPONSE = 1,
    OTA_CHECK_HTTP_ERROR  = 2,
    OTA_CHECK_BAD_RESPONSE= 3,
} ota_check_result_t;

// Update descriptor filled by ota_check_for_update().
// Layout mirrors the ~0x849-byte struct cleared/populated at 0x400dc83c.
typedef struct {
    bool     update_available;        // +0x00
    char     target_version[0x1f];    // +0x01 ("targetVersion"), NUL at +0x20
    uint8_t  _pad[3];
    uint32_t size;                    // +0x24 ("size")
    uint8_t  sha256[32];              // +0x28 ("checksum".value, SHA256, 64 hex -> 32 bytes)
    bool     checksum_valid;          // +0x48
    char     download_url[0x800];     // +0x49 ("downloadUrl", <= 2047 bytes)
} ota_update_info_t;

// Query the update service.
//   hardware_version : substituted into the URL  /* inferred param */
//   firmware_version : current running version (defaults to "" if NULL)
//   out              : filled on OTA_CHECK_OK (zeroed on any error)
//   http_status_out  : optional; receives HTTP status (-1 if none)
// @ 0x400dc83c  (tag "aws_update")
ota_check_result_t ota_check_for_update(const char *hardware_version,
                                        const char *firmware_version,
                                        ota_update_info_t *out,
                                        int *http_status_out);

// Human string for a result code. @ 0x400dc818
const char *ota_check_result_str(ota_check_result_t r);

// ---- download + install (ota.cpp extension) --------------------------------
// Download the offered image over HTTPS, stream it into the next OTA partition
// while hashing, verify the SHA-256 from the check (install is checksum-gated),
// set the boot partition, and optionally restart. @ 0x400e2b94
void ota_install(bool from_jobs, bool allow_restart);

// Pre-flight gate: run/defer/skip ota_install() based on charger + internet +
// BLE-session + free-heap conditions. Returns whether an attempt was made. @ 0x400e32fc
bool ota_update_gate(bool forced);

// Mark a log upload due ("scheduled" vs "requested"). @ 0x400e10d4
void log_upload_schedule(void);

// AWS IoT Jobs status report: {"status":…,"expectedVersion":…}. @ 0x400ef318
void jobs_report_status(const char *status, const char *expected_version);

#ifdef __cplusplus
}
#endif
