#include "netlog.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifndef CONFIG_CS_NETLOG_BUFFER_SIZE
#define CONFIG_CS_NETLOG_BUFFER_SIZE 12288
#endif

#define BUF_SIZE   CONFIG_CS_NETLOG_BUFFER_SIZE
#define NETLOG_LINE_MAX  256

static char             s_buf[BUF_SIZE];
static uint64_t         s_written;      /* total bytes ever appended */
static SemaphoreHandle_t s_lock;
static vprintf_like_t   s_orig;
static bool             s_ready;

static void append(const char *data, size_t len)
{
    if (len == 0) {
        return;
    }
    /* A single write longer than the ring can only keep its tail. */
    if (len > BUF_SIZE) {
        data += (len - BUF_SIZE);
        len = BUF_SIZE;
    }

    size_t head = (size_t)(s_written % BUF_SIZE);
    size_t first = BUF_SIZE - head;
    if (first > len) {
        first = len;
    }
    memcpy(s_buf + head, data, first);
    if (len > first) {
        memcpy(s_buf, data + first, len - first);
    }
    s_written += len;
}

/*
 * Must never call ESP_LOG* - that would recurse straight back into here.
 */
static int log_hook(const char *fmt, va_list args)
{
    if (s_ready) {
        va_list copy;
        va_copy(copy, args);
        char line[NETLOG_LINE_MAX];
        int n = vsnprintf(line, sizeof(line), fmt, copy);
        va_end(copy);

        if (n > 0) {
            size_t len = (n < (int)sizeof(line)) ? (size_t)n : sizeof(line) - 1;
            /*
             * Short wait, then give up. Losing a log line is strictly better
             * than stalling whichever task happened to be logging.
             */
            if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(10)) == pdTRUE) {
                append(line, len);
                xSemaphoreGive(s_lock);
            }
        }
    }
    return s_orig ? s_orig(fmt, args) : vprintf(fmt, args);
}

esp_err_t netlog_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    s_written = 0;
    s_ready = true;
    s_orig = esp_log_set_vprintf(log_hook);
    return ESP_OK;
}

size_t netlog_read(uint64_t since, char *out, size_t out_len,
                   uint64_t *next, uint64_t *dropped)
{
    if (!out || out_len == 0 || !s_ready) {
        if (next)    *next = 0;
        if (dropped) *dropped = 0;
        return 0;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    uint64_t head = s_written;
    uint64_t tail = (head > BUF_SIZE) ? (head - BUF_SIZE) : 0;

    uint64_t drop = 0;
    if (since < tail) {
        drop = tail - since;      /* reader fell behind; skip to oldest kept */
        since = tail;
    } else if (since > head) {
        since = head;             /* reader ahead of us: a reboot happened */
    }

    size_t avail = (size_t)(head - since);
    size_t n = (avail < out_len) ? avail : out_len;

    if (n) {
        size_t start = (size_t)(since % BUF_SIZE);
        size_t first = BUF_SIZE - start;
        if (first > n) {
            first = n;
        }
        memcpy(out, s_buf + start, first);
        if (n > first) {
            memcpy(out + first, s_buf, n - first);
        }
    }

    xSemaphoreGive(s_lock);

    if (next)    *next = since + n;
    if (dropped) *dropped = drop;
    return n;
}

uint64_t netlog_head(void)
{
    return s_written;
}

uint64_t netlog_tail(void)
{
    uint64_t head = s_written;
    return (head > BUF_SIZE) ? (head - BUF_SIZE) : 0;
}
