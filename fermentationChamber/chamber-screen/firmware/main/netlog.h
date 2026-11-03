/*
 * netlog.h - tee the ESP-IDF log into an in-memory ring buffer so it can be
 * read back over HTTP.
 *
 * Serial output is unaffected: the original vprintf handler is still called,
 * so `idf.py monitor` keeps working exactly as before.
 *
 * Readers identify their position with a monotonic byte offset rather than a
 * line count. That makes catching up after a dropped poll trivial, and lets
 * the server tell a client that fell too far behind exactly how much it
 * missed instead of silently skipping it.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t netlog_init(void);

/*
 * Copy log bytes written at or after `since` into `out`.
 *
 * Returns the number of bytes copied. On return:
 *   *next     - offset to pass on the following call
 *   *dropped  - bytes that were overwritten before this reader got to them
 *               (non-zero means the reader fell behind the ring)
 */
size_t netlog_read(uint64_t since, char *out, size_t out_len,
                   uint64_t *next, uint64_t *dropped);

/* Total bytes ever written - the offset a new reader should start from. */
uint64_t netlog_head(void);

/* Oldest offset still retained. */
uint64_t netlog_tail(void);
