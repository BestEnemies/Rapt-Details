#include "pill.h"

#include <string.h>
#include <math.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/util/util.h"

static const char *TAG = "pill";

/* Company ids seen in the wild: "RA" and "KE", both little endian. */
#define RAPT_CID_RA  0x4152
#define RAPT_CID_KE  0x454b

#define RAPT_MFG_LEN 25          /* full manufacturer-specific data length */

static pill_reading_t   s_sensors[PILL_MAX_SENSORS];
static int              s_count;
static SemaphoreHandle_t s_lock;
static uint8_t          s_own_addr_type;

/* ------------------------------------------------------------------ */
/* big-endian readers - the RAPT payload is BE after the company id     */

static inline uint16_t be16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static inline int16_t be16s(const uint8_t *p)
{
    return (int16_t)be16(p);
}

static inline float be32f(const uint8_t *p)
{
    uint32_t v = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                 ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
    float f;
    memcpy(&f, &v, sizeof(f));
    return f;
}

/* ------------------------------------------------------------------ */

static pill_reading_t *slot_for(const char *addr)
{
    for (int i = 0; i < s_count; i++) {
        if (!strcmp(s_sensors[i].addr, addr)) {
            return &s_sensors[i];
        }
    }
    if (s_count < PILL_MAX_SENSORS) {
        pill_reading_t *r = &s_sensors[s_count++];
        memset(r, 0, sizeof(*r));
        strlcpy(r->addr, addr, sizeof(r->addr));
        ESP_LOGI(TAG, "new sensor %s", addr);
        return r;
    }
    return NULL;     /* table full; ignore further sensors */
}

static bool parse_rapt(const uint8_t *d, int len, const char *addr, int rssi)
{
    if (len < RAPT_MFG_LEN) {
        return false;
    }
    uint16_t cid = (uint16_t)(d[0] | (d[1] << 8));
    if (cid != RAPT_CID_RA && cid != RAPT_CID_KE) {
        return false;
    }
    if (d[2] != 'P' || d[3] != 'T') {
        return false;
    }

    uint8_t version = d[4];
    if (version != 1 && version != 2) {
        return false;
    }

    pill_reading_t r = { 0 };
    r.version = version;

    if (version == 2) {
        r.velocity_valid   = (d[6] != 0);
        r.gravity_velocity = be32f(&d[7]);
    }

    r.celsius     = be16(&d[11]) / 128.0f - 273.15f;
    r.gravity     = be32f(&d[13]) / 1000.0f;
    r.accel_x     = be16s(&d[17]) / 16.0f;
    r.accel_y     = be16s(&d[19]) / 16.0f;
    r.accel_z     = be16s(&d[21]) / 16.0f;
    r.battery_pct = (int)lroundf(be16s(&d[23]) / 256.0f);
    r.rssi        = rssi;
    r.valid       = true;
    r.last_seen_us = esp_timer_get_time();

    xSemaphoreTake(s_lock, portMAX_DELAY);
    pill_reading_t *slot = slot_for(addr);
    if (slot) {
        strlcpy(r.addr, slot->addr, sizeof(r.addr));

        /* Carry the measured period forward, updating only across wakes. */
        r.interval_s = slot->interval_s;
        if (slot->valid) {
            int gap = (int)((r.last_seen_us - slot->last_seen_us) / 1000000);
            if (gap >= PILL_BURST_GAP_S) {
                /* Light smoothing - one long gap should not dominate. */
                r.interval_s = slot->interval_s
                             ? (slot->interval_s * 3 + gap) / 4
                             : gap;
                ESP_LOGD(TAG, "%s wake after %ds (period ~%ds)",
                         addr, gap, r.interval_s);
            }
        }
        *slot = r;
    }
    xSemaphoreGive(s_lock);
    return slot != NULL;
}

/* Walk the AD structures looking for manufacturer-specific data (type 0xFF). */
static void handle_adv(const struct ble_gap_disc_desc *disc)
{
    char addr[PILL_ADDR_STR_LEN];
    const uint8_t *a = disc->addr.val;
    snprintf(addr, sizeof(addr), "%02x:%02x:%02x:%02x:%02x:%02x",
             a[5], a[4], a[3], a[2], a[1], a[0]);

    const uint8_t *p = disc->data;
    int remaining = disc->length_data;

    while (remaining >= 2) {
        int field_len = p[0];
        if (field_len < 1 || field_len > remaining - 1) {
            return;                       /* malformed */
        }
        uint8_t type = p[1];
        if (type == BLE_HS_ADV_TYPE_MFG_DATA) {
            parse_rapt(&p[2], field_len - 1, addr, disc->rssi);
        }
        p         += field_len + 1;
        remaining -= field_len + 1;
    }
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        handle_adv(&event->disc);
        break;
    case BLE_GAP_EVENT_DISC_COMPLETE:
        ESP_LOGW(TAG, "scan ended (reason %d); restarting",
                 event->disc_complete.reason);
        /* Observer runs forever; if it ever stops, start it again. */
        {
            struct ble_gap_disc_params p = { 0 };
            p.passive = 1;
            p.filter_duplicates = 0;
            ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &p, gap_event, NULL);
        }
        break;
    default:
        break;
    }
    return 0;
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_util_ensure_addr: %d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto: %d", rc);
        return;
    }

    struct ble_gap_disc_params p = { 0 };
    p.passive = 1;            /* the Pill broadcasts; never scan-request it */
    p.filter_duplicates = 0;  /* we want every update, not just new devices */

    rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_disc: %d", rc);
        return;
    }
    ESP_LOGI(TAG, "scanning (passive, observer)");
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "nimble reset, reason %d", reason);
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t pill_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

int pill_count(void)
{
    return s_count;
}

bool pill_get(int idx, pill_reading_t *out)
{
    if (!out || !s_lock || idx < 0 || idx >= s_count) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_sensors[idx];
    xSemaphoreGive(s_lock);
    return out->valid;
}

bool pill_get_by_addr(const char *addr, pill_reading_t *out)
{
    if (!out || !s_lock) {
        return false;          /* before pill_start(): simply no data */
    }
    bool found = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (addr && addr[0]) {
        for (int i = 0; i < s_count; i++) {
            if (!strcasecmp(s_sensors[i].addr, addr) && s_sensors[i].valid) {
                *out = s_sensors[i];
                found = true;
                break;
            }
        }
    } else {
        int64_t newest = -1;
        for (int i = 0; i < s_count; i++) {
            if (s_sensors[i].valid && s_sensors[i].last_seen_us > newest) {
                newest = s_sensors[i].last_seen_us;
                *out = s_sensors[i];
                found = true;
            }
        }
    }

    xSemaphoreGive(s_lock);
    return found;
}

int pill_age_seconds(const char *addr)
{
    pill_reading_t r;
    if (!pill_get_by_addr(addr, &r)) {
        return -1;
    }
    return (int)((esp_timer_get_time() - r.last_seen_us) / 1000000);
}
