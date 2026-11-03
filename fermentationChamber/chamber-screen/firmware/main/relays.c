#include "relays.h"
#include "board.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "relays";

static const gpio_num_t s_pin[RELAY_COUNT] = {
    [RELAY_HEATER] = BOARD_GPIO_HEATER,
    [RELAY_COOLER] = BOARD_GPIO_COOLER,
    [RELAY_FAN]    = BOARD_GPIO_FAN,
    [RELAY_LIGHT]  = BOARD_GPIO_LIGHT,
    [RELAY_BUZZER] = BOARD_GPIO_BUZZER,
};

static const char *s_name[RELAY_COUNT] = {
    "heater", "cooler", "fan", "light", "buzzer"
};

typedef struct {
    bool     on;
    int64_t  changed_us;      /* when it last changed state          */
    uint64_t runtime_us;      /* accumulated energised time          */
    uint32_t starts;          /* off -> on transitions               */
} relay_state_t;

static relay_state_t     s_state[RELAY_COUNT];
static relays_config_t   s_cfg;
static SemaphoreHandle_t s_lock;
static int64_t           s_boot_us;
static bool              s_simultaneous;   /* reheat dehumidification */

static inline int64_t now_us(void) { return esp_timer_get_time(); }

static void apply(relay_t r, bool on)
{
    if (s_state[r].on == on) {
        return;
    }
    gpio_set_level(s_pin[r], on ? 1 : 0);

    int64_t t = now_us();
    if (s_state[r].on) {
        s_state[r].runtime_us += (uint64_t)(t - s_state[r].changed_us);
    } else {
        s_state[r].starts++;
    }
    s_state[r].on = on;
    s_state[r].changed_us = t;

    ESP_LOGI(TAG, "%s -> %s", s_name[r], on ? "ON" : "OFF");
}

static uint32_t seconds_since_change(relay_t r)
{
    return (uint32_t)((now_us() - s_state[r].changed_us) / 1000000);
}

esp_err_t relays_init(const relays_config_t *cfg)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }

    s_cfg = cfg ? *cfg : (relays_config_t)RELAYS_DEFAULT_CONFIG();
    s_boot_us = now_us();

    uint64_t mask = 0;
    for (int i = 0; i < RELAY_COUNT; i++) {
        mask |= (1ULL << s_pin[i]);
    }

    gpio_config_t io = {
        .pin_bit_mask = mask,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        return err;
    }

    /* Everything off, explicitly, before anything else runs. */
    for (int i = 0; i < RELAY_COUNT; i++) {
        gpio_set_level(s_pin[i], 0);
        s_state[i].on         = false;
        s_state[i].changed_us = s_boot_us;
        s_state[i].runtime_us = 0;
        s_state[i].starts     = 0;
    }

    ESP_LOGI(TAG, "init: min_off=%us min_on=%us mode_switch=%us",
             (unsigned)s_cfg.compressor_min_off_s,
             (unsigned)s_cfg.compressor_min_on_s,
             (unsigned)s_cfg.mode_switch_delay_s);
    return ESP_OK;
}

void relays_configure(const relays_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg = *cfg;
    xSemaphoreGive(s_lock);
}

esp_err_t relays_set(relay_t r, bool on)
{
    if (r >= RELAY_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (r == RELAY_HEATER || r == RELAY_COOLER) {
        return ESP_ERR_INVALID_ARG;   /* must go through the guarded path */
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    apply(r, on);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

bool relays_get(relay_t r)
{
    return (r < RELAY_COUNT) ? s_state[r].on : false;
}

/* How long until `r` may switch to `on`? 0 means now. Caller holds the lock. */
static uint32_t blocked_for(relay_t r, bool on)
{
    relay_t other = (r == RELAY_COOLER) ? RELAY_HEATER : RELAY_COOLER;

    if (on) {
        /*
         * Normally: never both, and dwell after the opposing output stops.
         * Reheat dehumidification deliberately suspends exactly these two
         * rules - and nothing else. The compressor timings below still apply.
         */
        if (!s_simultaneous) {
            if (s_state[other].on) {
                return s_cfg.mode_switch_delay_s ? s_cfg.mode_switch_delay_s : 1;
            }
            uint32_t since_other = seconds_since_change(other);
            if (since_other < s_cfg.mode_switch_delay_s) {
                return s_cfg.mode_switch_delay_s - since_other;
            }
        }
        /* Compressor anti-short-cycle. */
        if (r == RELAY_COOLER && !s_state[r].on) {
            uint32_t off_for = seconds_since_change(r);
            if (off_for < s_cfg.compressor_min_off_s) {
                return s_cfg.compressor_min_off_s - off_for;
            }
        }
    } else {
        /* Minimum run time before the compressor may stop. */
        if (r == RELAY_COOLER && s_state[r].on) {
            uint32_t on_for = seconds_since_change(r);
            if (on_for < s_cfg.compressor_min_on_s) {
                return s_cfg.compressor_min_on_s - on_for;
            }
        }
    }
    return 0;
}

static esp_err_t request(relay_t r, bool on)
{
    esp_err_t ret = ESP_OK;
    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (s_state[r].on == on) {
        goto done;                       /* already there, nothing to guard */
    }
    if (blocked_for(r, on) > 0) {
        ret = ESP_ERR_INVALID_STATE;
        goto done;
    }
    apply(r, on);

done:
    xSemaphoreGive(s_lock);
    return ret;
}

esp_err_t relays_request_cool(bool on) { return request(RELAY_COOLER, on); }
esp_err_t relays_request_heat(bool on) { return request(RELAY_HEATER, on); }

uint32_t relays_cool_blocked_for(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t v = blocked_for(RELAY_COOLER, !s_state[RELAY_COOLER].on);
    xSemaphoreGive(s_lock);
    return v;
}

uint32_t relays_heat_blocked_for(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t v = blocked_for(RELAY_HEATER, !s_state[RELAY_HEATER].on);
    xSemaphoreGive(s_lock);
    return v;
}

void relays_emergency_stop(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ESP_LOGW(TAG, "emergency stop - heater and cooler de-energised");
    apply(RELAY_HEATER, false);
    apply(RELAY_COOLER, false);
    /*
     * Revoke the reheat permission too. Whatever was supervising it has, by
     * definition, just lost control - it must ask again.
     */
    s_simultaneous = false;
    xSemaphoreGive(s_lock);
}

void relays_allow_simultaneous(bool allow)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_simultaneous != allow) {
        ESP_LOGW(TAG, "simultaneous heat+cool %s",
                 allow ? "PERMITTED (reheat dehumidification)" : "revoked");
        s_simultaneous = allow;
        if (!allow && s_state[RELAY_HEATER].on && s_state[RELAY_COOLER].on) {
            /* Leaving reheat: drop the heater, keep cooling. */
            apply(RELAY_HEATER, false);
        }
    }
    xSemaphoreGive(s_lock);
}

bool relays_simultaneous_allowed(void)
{
    return s_simultaneous;
}

uint32_t relays_runtime_seconds(relay_t r)
{
    if (r >= RELAY_COUNT) {
        return 0;
    }
    uint64_t total = s_state[r].runtime_us;
    if (s_state[r].on) {
        total += (uint64_t)(now_us() - s_state[r].changed_us);
    }
    return (uint32_t)(total / 1000000);
}

uint32_t relays_start_count(relay_t r)
{
    return (r < RELAY_COUNT) ? s_state[r].starts : 0;
}

uint32_t relays_total_runtime_seconds(void)
{
    return (uint32_t)((now_us() - s_boot_us) / 1000000);
}
