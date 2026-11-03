// web_config.h — RAPT Pill 2.0.0, reverse-engineered (readable reconstruction)
//
// The soft-AP configuration portal: a single POST dispatcher that the
// esp32-wifi-manager HTTP server calls for the device's own `/*.json`
// endpoints. Settings arrive in "X-Custom-*" request headers (or the body)
// and are persisted to NVS; most handlers reply with a small JSON object.
//
// Provenance: app_2.0.0.bin, log tag "pill_ap".  See docs/RECONSTRUCTION_NOTES.md.
#pragma once
#include "esp_http_server.h"

// Registered as the POST URI handler for the Pill's config endpoints.
// Returns ESP_OK. (Ghidra: FUN_400e40cc)
esp_err_t pill_config_post_handler(httpd_req_t *req);
