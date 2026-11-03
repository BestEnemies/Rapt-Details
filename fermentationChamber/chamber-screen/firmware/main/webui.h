/*
 * webui.h - development web UI: status, live log tail and OTA upload.
 *
 * There is NO authentication. This exists to make the develop/flash/observe
 * loop fast on a bench, not to be exposed to a network you do not control.
 *
 * Routes:
 *   GET  /                          status page
 *   GET  /api/status                JSON snapshot
 *   GET  /api/log?since=<offset>    log bytes; offset returned in headers
 *   POST /api/relay?name=fan&on=1   toggle an output
 *   POST /api/ota                   raw .bin body, writes the inactive slot
 */

#pragma once

#include "esp_err.h"

esp_err_t webui_start(void);
esp_err_t webui_stop(void);

/*
 * Cancel the pending rollback and mark this image good.
 *
 * With CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE, an OTA image boots in
 * PENDING_VERIFY. If it reboots without calling this, the bootloader reverts
 * to the previous slot. Call it only once the device is demonstrably healthy
 * - that is what makes a bad OTA self-recovering instead of a bench trip.
 */
void webui_mark_app_valid(void);
