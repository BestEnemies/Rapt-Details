#include "ui.h"
#include "board.h"
#include "display.h"
#include "relays.h"
#include "thermostat.h"
#include "pill.h"
#include "humidity.h"
#include "alarms.h"

#include <stdio.h>
#include <math.h>

#include "esp_log.h"
#include "esp_lvgl_port.h"

static const char *TAG = "ui";

/* Panel palette - see the header for why these differ from the web UI. */
#define C_BG        lv_color_hex(0x0e141a)
#define C_SURFACE   lv_color_hex(0x1e2a36)
#define C_RULE      lv_color_hex(0x3a4a5a)
#define C_TEXT      lv_color_hex(0xe6ecf1)
#define C_MUTED     lv_color_hex(0x8fa3b4)
#define C_COOL      lv_color_hex(0x5ea2ce)
#define C_WARM      lv_color_hex(0xe07a50)
#define C_ACCENT    lv_color_hex(0xdc9b42)
#define C_OK        lv_color_hex(0x6fbf8e)
#define C_ALARM     lv_color_hex(0xd8483a)

static lv_obj_t *s_screen;
static lv_obj_t *s_banner, *s_banner_lbl, *s_banner_detail;
static lv_obj_t *s_big, *s_unit, *s_target, *s_footer, *s_outputs;
static ui_screen_t s_current;

/*
 * Live orientation. Seeded from the build-time default and changed at runtime
 * by ui_set_mirror() so the correct handedness can be found on the bench.
 */
static bool s_mirror_x = UI_DEFAULT_MIRROR_X;
static bool s_mirror_y = UI_DEFAULT_MIRROR_Y;

/* ------------------------------------------------------------------ */

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font,
                            lv_color_t colour, lv_align_t align,
                            lv_coord_t x, lv_coord_t y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, colour, 0);
    lv_obj_align(l, align, x, y);
    return l;
}

static void build_main(void)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, C_BG, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);

    /* state banner */
    s_banner = lv_obj_create(s_screen);
    lv_obj_set_size(s_banner, BOARD_LCD_UI_W - 28, 34);
    lv_obj_align(s_banner, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_set_style_radius(s_banner, 4, 0);
    lv_obj_set_style_border_width(s_banner, 0, 0);
    lv_obj_set_style_pad_all(s_banner, 0, 0);
    lv_obj_clear_flag(s_banner, LV_OBJ_FLAG_SCROLLABLE);

    s_banner_lbl = make_label(s_banner, &lv_font_montserrat_16, C_COOL,
                              LV_ALIGN_LEFT_MID, 10, 0);
    s_banner_detail = make_label(s_banner, &lv_font_montserrat_14, C_MUTED,
                                 LV_ALIGN_RIGHT_MID, -10, 0);

    /*
     * The design wants 80px here; LVGL's largest built-in is 48. Readable,
     * but noticeably smaller than intended - see the header note.
     */
    /* Landscape: the reading sits left, the target and outputs stack right. */
    s_big  = make_label(s_screen, &lv_font_montserrat_48, C_TEXT,
                        LV_ALIGN_LEFT_MID, 16, -8);
    s_unit = make_label(s_screen, &lv_font_montserrat_20, C_MUTED,
                        LV_ALIGN_LEFT_MID, 150, 2);

    s_target = make_label(s_screen, &lv_font_montserrat_20, C_TEXT,
                          LV_ALIGN_RIGHT_MID, -16, -14);
    s_outputs = make_label(s_screen, &lv_font_montserrat_16, C_MUTED,
                           LV_ALIGN_RIGHT_MID, -16, 16);

    s_footer  = make_label(s_screen, &lv_font_montserrat_14, C_MUTED,
                           LV_ALIGN_BOTTOM_LEFT, 16, -12);
}

/* ------------------------------------------------------------------ */

static void refresh_main(void)
{
    thermostat_config_t cfg;
    thermostat_status_t st;
    thermostat_get_config(&cfg);
    thermostat_get_status(&st);

    /* An alarm outranks the control state - it is why you looked up. */
    alarm_id_t aid;
    bool alarmed = alarms_highest(&aid);

    const char *label = "IDLE";
    lv_color_t colour = C_MUTED;
    lv_color_t tint   = C_SURFACE;

    if (alarmed) {
        label = "ALARM";  colour = C_ALARM; tint = lv_color_hex(0x33110e);
    } else if (!cfg.enabled) {
        label = "OFF";    colour = C_MUTED; tint = C_SURFACE;
    } else {
        switch (st.state) {
        case THERMO_COOLING:       label = "COOLING"; colour = C_COOL;   tint = lv_color_hex(0x16283a); break;
        case THERMO_HEATING:       label = "HEATING"; colour = C_WARM;   tint = lv_color_hex(0x33201a); break;
        case THERMO_DEHUMIDIFYING: label = "DEHUMID"; colour = C_ACCENT; tint = lv_color_hex(0x33280f); break;
        case THERMO_FAULT:         label = "FAULT";   colour = C_ALARM;  tint = lv_color_hex(0x33110e); break;
        default:                   label = "IDLE";    colour = C_MUTED;  tint = C_SURFACE; break;
        }
    }

    lv_obj_set_style_bg_color(s_banner, tint, 0);
    lv_label_set_text(s_banner_lbl, label);
    lv_obj_set_style_text_color(s_banner_lbl, colour, 0);

    if (alarmed) {
        lv_label_set_text(s_banner_detail, alarms_key(aid));
    } else if (st.state == THERMO_COOLING || st.state == THERMO_HEATING) {
        char b[16];
        snprintf(b, sizeof(b), "%lus", (unsigned long)st.state_seconds);
        lv_label_set_text(s_banner_detail, b);
    } else {
        lv_label_set_text(s_banner_detail, "");
    }

    /*
     * Cached state only. An earlier version called temp_probe_read() here,
     * which fires 128 ADC conversions inside a 1 Hz LVGL timer callback - I/O
     * on the render task, on a small stack. A UI draws what it is given.
     */
    char big[16];
    if (st.temp_valid) {
        snprintf(big, sizeof(big), "%.1f", st.temp_c);
    } else {
        snprintf(big, sizeof(big), "--.-");
    }
    lv_label_set_text(s_big, big);
    lv_label_set_text(s_unit, "C");

    char tgt[32];
    snprintf(tgt, sizeof(tgt), "TARGET %.1f", cfg.target_c);
    lv_label_set_text(s_target, tgt);

    /* outputs, as initials - readable further away than icons at this size */
    char outs[32];
    snprintf(outs, sizeof(outs), "%s %s %s %s",
             relays_get(RELAY_COOLER) ? "COOL" : "",
             relays_get(RELAY_HEATER) ? "HEAT" : "",
             relays_get(RELAY_FAN)    ? "FAN"  : "",
             relays_get(RELAY_LIGHT)  ? "LAMP" : "");
    lv_label_set_text(s_outputs, outs);

    pill_reading_t p;
    if (pill_get_by_addr(cfg.pill_addr, &p)) {
        char f[40];
        snprintf(f, sizeof(f), "%s  %.3f SG  %.1fC",
                 cfg.source == TEMP_SOURCE_PILL ? "PILL" : "PROBE",
                 p.gravity, p.celsius);
        lv_label_set_text(s_footer, f);
    } else {
        lv_label_set_text(s_footer,
                          cfg.source == TEMP_SOURCE_PILL ? "waiting for pill" : "PROBE");
    }
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    refresh_main();
}

/* ------------------------------------------------------------------ */

esp_err_t ui_init(void)
{
    if (!display_panel() || !display_io()) {
        ESP_LOGW(TAG, "no display; panel UI disabled");
        return ESP_ERR_INVALID_STATE;
    }

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_stack    = 8192;   /* default 4 KB is tight for real screens */
    port_cfg.task_priority = 3;      /* below control and networking */
    port_cfg.timer_period_ms = 20;
    esp_err_t err = lvgl_port_init(&port_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lvgl_port_init: %s", esp_err_to_name(err));
        return err;
    }

    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle     = display_io(),
        .panel_handle  = display_panel(),
        .buffer_size   = BOARD_LCD_UI_W * 40,
        .double_buffer = false,
        /*
         * These are the POST-rotation dimensions, not the panel's native ones.
         * lvgl_port_add_disp() passes them straight to lv_display_create() and
         * separately programs the panel's MADCTL from .rotation below, so the
         * two have to be stated consistently: a 240x320 panel with swap_xy
         * presents as 320x240, and that is what LVGL must be told it has.
         */
        .hres          = BOARD_LCD_UI_W,
        .vres          = BOARD_LCD_UI_H,
        .monochrome    = false,
        /*
         * The panel is mounted rotated - buttons along the bottom edge - so
         * the content has to be turned 90 degrees to match. swap_xy plus one
         * mirror gives 90; WHICH mirror decides clockwise vs anticlockwise,
         * and that is a fact about how the glass is glued down, not something
         * derivable from the datasheet. ui_set_mirror() re-programs it live so
         * the right answer can be found by looking rather than by rebuilding.
         */
        .rotation = {
            .swap_xy  = true,
            .mirror_x = UI_DEFAULT_MIRROR_X,
            .mirror_y = UI_DEFAULT_MIRROR_Y,
        },
        .color_format  = LV_COLOR_FORMAT_RGB565,
        .flags = {
            /*
             * The draw buffer lives in PSRAM, not internal DRAM. At
             * 240 x 40 x 2 bytes it is ~19 KB, and keeping it internal
             * overflowed dram0_0_seg - the WiFi, BLE and TLS stacks have
             * already claimed most of it.
             *
             * PSRAM cannot be a DMA source for SPI on the ESP32, so the port
             * copies instead. For a status panel refreshing once a second
             * that costs nothing worth measuring.
             */
            .buff_dma    = false,
            .buff_spiram = true,
            /*
             * The panel takes RGB565 most-significant byte first. Letting the
             * port swap is the right place for it - doing it per-pixel in
             * application code, as the bring-up fill did, does not scale.
             */
            /*
             * FALSE, not true. The bring-up display_fill() swapped in
             * software and needed to; the port swaps on the same path, so
             * asking for it here as well double-swaps. Empirically settled -
             * layout was correct and only the colours were wrong.
             */
            .swap_bytes  = false,
        },
    };
    if (!lvgl_port_add_disp(&disp_cfg)) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        return ESP_FAIL;
    }

    if (lvgl_port_lock(0)) {
        build_main();
        lv_screen_load(s_screen);
        refresh_main();
        lv_timer_create(tick_cb, 1000, NULL);
        lvgl_port_unlock();
    }

    s_current = UI_SCREEN_MAIN;
    ESP_LOGI(TAG, "panel UI up (%dx%d landscape, mirror x=%d y=%d)",
             BOARD_LCD_UI_W, BOARD_LCD_UI_H, s_mirror_x, s_mirror_y);
    return ESP_OK;
}

void ui_set_mirror(bool mirror_x, bool mirror_y)
{
    if (!s_screen || !display_panel()) {
        ESP_LOGW(TAG, "no panel UI; ignoring mirror change");
        return;
    }
    s_mirror_x = mirror_x;
    s_mirror_y = mirror_y;
    /*
     * Goes straight at the panel rather than through the port: the port only
     * re-applies .rotation on an LVGL resolution change, and the resolution is
     * not changing - only which corner the origin sits in.
     */
    esp_lcd_panel_mirror(display_panel(), mirror_x, mirror_y);
    if (lvgl_port_lock(0)) {
        lv_obj_invalidate(lv_screen_active());
        lvgl_port_unlock();
    }
    ESP_LOGI(TAG, "mirror now x=%d y=%d", mirror_x, mirror_y);
}

void ui_get_mirror(bool *mirror_x, bool *mirror_y)
{
    if (mirror_x) *mirror_x = s_mirror_x;
    if (mirror_y) *mirror_y = s_mirror_y;
}

void ui_next_screen(void) { /* further screens still to be built */ }
void ui_prev_screen(void) { }
ui_screen_t ui_current_screen(void) { return s_current; }
