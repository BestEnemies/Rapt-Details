#include "buttons.h"
#include "board.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "buttons";

#define POLL_INTERVAL_MS   10
#define DEBOUNCE_MS        30
#define LONG_PRESS_MS      1200

static const gpio_num_t s_pin[BTN_COUNT] = {
    [BTN_UP]     = BOARD_GPIO_BTN_UP,
    [BTN_DOWN]   = BOARD_GPIO_BTN_DOWN,
    [BTN_SELECT] = BOARD_GPIO_BTN_SELECT,
    [BTN_BACK]   = BOARD_GPIO_BTN_BACK,
    [BTN_DOOR]   = BOARD_GPIO_DOOR,
};

static const char *s_name[BTN_COUNT] = {
    "up", "down", "select", "back", "door"
};

typedef struct {
    bool     stable;        /* debounced state: true = pressed/active   */
    bool     last_raw;
    int64_t  last_change_us;
    int64_t  pressed_at_us;
    bool     long_fired;
} btn_state_t;

static btn_state_t     s_state[BTN_COUNT];
static QueueHandle_t   s_queue;

static inline bool raw_active(button_t b)
{
    return gpio_get_level(s_pin[b]) == BOARD_BUTTON_ACTIVE_LEVEL;
}

static void emit(button_t b, button_event_type_t type, uint32_t held_ms)
{
    button_event_t ev = { .button = b, .type = type, .held_ms = held_ms };
    /* Drop rather than block - a full queue means the consumer has stalled. */
    if (xQueueSend(s_queue, &ev, 0) != pdTRUE) {
        ESP_LOGW(TAG, "event queue full, dropped %s", s_name[b]);
    }
}

static void poll_task(void *arg)
{
    (void)arg;
    for (;;) {
        int64_t now = esp_timer_get_time();

        for (int b = 0; b < BTN_COUNT; b++) {
            bool raw = raw_active((button_t)b);

            if (raw != s_state[b].last_raw) {
                s_state[b].last_raw       = raw;
                s_state[b].last_change_us = now;
                continue;                       /* wait out the bounce */
            }

            bool settled =
                (now - s_state[b].last_change_us) >= (DEBOUNCE_MS * 1000);

            if (settled && raw != s_state[b].stable) {
                s_state[b].stable = raw;
                if (raw) {
                    s_state[b].pressed_at_us = now;
                    s_state[b].long_fired    = false;
                    emit((button_t)b, BTN_EVENT_PRESSED, 0);
                } else {
                    uint32_t held =
                        (uint32_t)((now - s_state[b].pressed_at_us) / 1000);
                    emit((button_t)b, BTN_EVENT_RELEASED, held);
                }
            }

            /* Long-press fires once while still held. Not for the door. */
            if (b != BTN_DOOR && s_state[b].stable && !s_state[b].long_fired) {
                uint32_t held =
                    (uint32_t)((now - s_state[b].pressed_at_us) / 1000);
                if (held >= LONG_PRESS_MS) {
                    s_state[b].long_fired = true;
                    emit((button_t)b, BTN_EVENT_LONG_PRESS, held);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

esp_err_t buttons_init(void)
{
    s_queue = xQueueCreate(16, sizeof(button_event_t));
    if (!s_queue) {
        return ESP_ERR_NO_MEM;
    }

    uint64_t mask = 0;
    for (int i = 0; i < BTN_COUNT; i++) {
        mask |= (1ULL << s_pin[i]);
    }

    /*
     * No internal pull-ups requested. GPIO 35/36/39 physically cannot provide
     * them, and the board fits external ones; enabling them on 13/15 only
     * would make those two behave differently from the rest.
     */
    gpio_config_t io = {
        .pin_bit_mask = mask,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        return err;
    }

    int64_t now = esp_timer_get_time();
    for (int i = 0; i < BTN_COUNT; i++) {
        bool raw = raw_active((button_t)i);
        s_state[i].stable         = raw;
        s_state[i].last_raw       = raw;
        s_state[i].last_change_us = now;
        s_state[i].long_fired     = true;   /* no long-press for a held-at-boot key */
    }

    if (xTaskCreate(poll_task, "btn_poll", 2560, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "init: 5 inputs, active-%s",
             BOARD_BUTTON_ACTIVE_LEVEL ? "high" : "low");
    return ESP_OK;
}

bool buttons_poll(button_event_t *out, uint32_t wait_ms)
{
    if (!out) {
        return false;
    }
    return xQueueReceive(s_queue, out, pdMS_TO_TICKS(wait_ms)) == pdTRUE;
}

bool buttons_is_down(button_t b)
{
    return (b < BTN_COUNT) ? s_state[b].stable : false;
}

bool buttons_door_is_open(void)
{
    /*
     * Verified on hardware 2026-08-31: with the door switch physically open,
     * GPIO35 sits at the active level and this returns true. So active = open.
     */
    return s_state[BTN_DOOR].stable;
}

const char *buttons_name(button_t b)
{
    return (b < BTN_COUNT) ? s_name[b] : "?";
}
