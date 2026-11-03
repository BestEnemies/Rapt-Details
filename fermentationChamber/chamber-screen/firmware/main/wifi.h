/*
 * wifi.h - station-mode connection for the development web UI and OTA.
 *
 * Deliberately non-fatal. A fermentation controller that stops regulating
 * temperature because it cannot reach an access point is worse than one that
 * carries on regulating without a network, so a failure here is logged and
 * the controller continues.
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

esp_err_t wifi_start(void);

/* True once an IP has been obtained. */
bool wifi_is_connected(void);

/* Dotted-quad address, or "0.0.0.0" when not connected. */
const char *wifi_ip_str(void);

/* Last RSSI in dBm, 0 when unknown. Matches the stock telemetry field. */
int wifi_rssi(void);
