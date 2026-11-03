/*
 * main.c - bring-up firmware for the RAPT FridgeG5 controller.
 *
 * This is deliberately a hardware verification build, not a controller. It
 * exists to confirm every recovered pin against real hardware before anything
 * is built on top.
 *
 * Buttons are OBSERVATION ONLY: each press, release and long-press is logged
 * with its duration, and nothing else happens. Relay control lives in the web
 * UI, so handling the panel during testing can never energise an output by
 * accident.
 *
 * A status line prints once a second with every input, every output and the
 * probe reading.
 *
 * Nothing here drives the compressor from temperature. The control loop and
 * the KegLand cloud client are deliberately absent - add them once the pin
 * map is confirmed and the thermistor curve is calibrated.
 */

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board.h"
#include "relays.h"
#include "buttons.h"
#include "temp_probe.h"
#include "display.h"
#include "netlog.h"
#include "wifi.h"
#include "webui.h"
#include "pill.h"
#include "thermostat.h"
#include "humidity.h"
#include "ui.h"

static const char *TAG = "main";

static const char *event_name(button_event_type_t t)
{
    switch (t) {
    case BTN_EVENT_PRESSED:    return "PRESSED";
    case BTN_EVENT_RELEASED:   return "released";
    case BTN_EVENT_LONG_PRESS: return "LONG PRESS";
    default:                   return "?";
    }
}

static void print_status(void)
{
    probe_reading_t p;
    if (temp_probe_read(&p) != ESP_OK) {
        ESP_LOGE(TAG, "probe read failed");
        return;
    }

    printf("up=%d down=%d select=%d back=%d door=%d | ",
           buttons_is_down(BTN_UP), buttons_is_down(BTN_DOWN),
           buttons_is_down(BTN_SELECT), buttons_is_down(BTN_BACK),
           buttons_is_down(BTN_DOOR));

    printf("heat=%d cool=%d fan=%d light=%d buzz=%d | ",
           relays_get(RELAY_HEATER), relays_get(RELAY_COOLER),
           relays_get(RELAY_FAN), relays_get(RELAY_LIGHT),
           relays_get(RELAY_BUZZER));

    if (p.status == PROBE_OK) {
        printf("adc=%4d %4dmV %8.1fohm %6.2fC(uncal)\n",
               p.raw_mean, p.millivolts, p.resistance, p.celsius);
    } else {
        printf("probe FAULT: %s (raw=%d)\n",
               temp_probe_status_str(p.status), p.raw_mean);
    }
}

void app_main(void)
{
    /* First, so everything logged from here on is also readable over HTTP. */
    ESP_ERROR_CHECK(netlog_init());

    ESP_LOGI(TAG, "RAPT FridgeG5 - bring-up firmware");
    ESP_LOGI(TAG, "reset reason %d, %luKB free",
             (int)esp_reset_reason(),
             (unsigned long)(esp_get_free_heap_size() / 1024));

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /*
         * Deliberately NOT erasing. On this hardware nvs holds the stock
         * device registration and OAuth refresh token - erasing it forces a
         * re-pair against the RAPT portal. Report and continue instead.
         */
        ESP_LOGW(TAG, "nvs needs erase; refusing to do it automatically "
                      "(it holds your device registration)");
    } else {
        ESP_ERROR_CHECK(err);
    }

    relays_config_t rcfg = RELAYS_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(relays_init(&rcfg));
    ESP_ERROR_CHECK(buttons_init());
    ESP_ERROR_CHECK(temp_probe_init());

    /* Display bring-up is allowed to fail - DC/RST are unverified. */
    if (display_init() == ESP_OK) {
        uint8_t id[4] = {0};
        if (display_probe_id(id, sizeof(id)) == ESP_OK) {
            ESP_LOGI(TAG, "panel id: %02x %02x %02x %02x",
                     id[0], id[1], id[2], id[3]);
        }
        /*
         * NB: the panel UI is started later, not here. It reads from the
         * thermostat and the Pill scanner, so it must come up after them -
         * doing it here dereferenced their mutexes before they existed.
         */
    } else {
        ESP_LOGW(TAG, "display init failed; continuing without it");
    }

    /* BLE observer for RAPT Pill / RAPT Temp. Non-fatal if it fails. */
    if (pill_start() != ESP_OK) {
        ESP_LOGW(TAG, "BLE scanning unavailable; Pill readings disabled");
    }

    /*
     * Humidity sensor on the spare header. Absence is entirely normal - the
     * header is unpopulated on a stock board - so this is informational, and
     * the thermostat simply never enters reheat without it.
     */
    if (humidity_init() != ESP_OK) {
        ESP_LOGI(TAG, "no humidity sensor; dehumidification unavailable");
    }

    /*
     * Thermostat. Config persists in NVS; defaults are used on a fresh board,
     * and it starts DISABLED so nothing switches until you ask it to.
     */
    thermostat_config_t tcfg = THERMOSTAT_DEFAULT_CONFIG();
    esp_err_t terr = thermostat_load(&tcfg);
    if (terr == ESP_OK) {
        ESP_LOGI(TAG, "loaded saved thermostat config");
    } else {
        /* load() leaves tcfg untouched on failure, so the defaults stand. */
        ESP_LOGI(TAG, "using default thermostat config (%s)",
                 esp_err_to_name(terr));
    }
    ESP_ERROR_CHECK(thermostat_start(&tcfg));

    /*
     * Network BEFORE the panel UI, deliberately.
     *
     * This used to sit after ui_init(), on the reasoning that chamber control
     * must never depend on WiFi. That reasoning is still right, but it put the
     * single riskiest call in the boot path - display and LVGL bring-up - in
     * front of the only remote recovery channel there is. A panic inside
     * ui_init() therefore took the network with it, and once BOTH OTA slots
     * held such a build there was no way back in without a cable. That is
     * exactly how this board was bricked on 2026-08-31.
     *
     * Networking first costs nothing: it starts tasks and returns, and the
     * relay interlocks are already armed by thermostat_start() above.
     */
#if CONFIG_CS_WEBUI_ENABLE
    if (wifi_start() == ESP_OK) {
        if (webui_start() == ESP_OK) {
            ESP_LOGI(TAG, "bench UI: http://%s/", wifi_ip_str());
        }
    } else {
        ESP_LOGW(TAG, "no network - bench UI unavailable, hardware unaffected");
    }
#endif

    /*
     * Panel UI last: it reads thermostat, pill, humidity and alarm state, so
     * every one of those must already exist.
     */
    if (ui_init() != ESP_OK) {
        ESP_LOGW(TAG, "panel UI unavailable; display left blank");
    }

    /*
     * Only NOW is this image declared good - after the display too, so a build
     * that panics in ui_init() still rolls back on the next boot. Rollback and
     * remote access are both preserved: the network is up either way, and the
     * validation still covers everything.
     */
#if CONFIG_CS_WEBUI_ENABLE
    webui_mark_app_valid();
#endif

    ESP_LOGI(TAG, "ready - buttons report only; relay control is in the web UI");

    int64_t last_print = 0;

    for (;;) {
        /*
         * Buttons are observation-only: they report, they do not act. Relay
         * control lives in the web UI, so poking at the panel while testing
         * can never energise anything by accident.
         */
        button_event_t ev;
        while (buttons_poll(&ev, 0)) {
            if (ev.held_ms) {
                ESP_LOGI(TAG, "button %s %s (%ums)",
                         buttons_name(ev.button), event_name(ev.type),
                         (unsigned)ev.held_ms);
            } else {
                ESP_LOGI(TAG, "button %s %s",
                         buttons_name(ev.button), event_name(ev.type));
            }
        }

        int64_t now = esp_timer_get_time();
        if (now - last_print >= 1000000) {
            last_print = now;
            print_status();
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
