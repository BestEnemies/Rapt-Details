/*
 * ui.h - LVGL panel UI.
 *
 * Implements the designs at
 * https://claude.ai/code/artifact/29af143e-d992-42b6-aadb-2c37e9cec8f5
 *
 * Palette note: the neutrals here are NOT the web UI's. On this panel
 * #151d25 on #0e141a quantises to roughly 1.04:1 in RGB565 and is invisible,
 * so the surfaces and rules are lifted. The semantic colours - cooling,
 * heating, dehumidify, ok - are identical to the web UI so the two agree.
 *
 * Type: LVGL's built-in Montserrat. The design's 80px numeral has no built-in
 * equivalent (LVGL ships up to 48), so the main reading currently renders at
 * 48 and wants a font generated with lv_font_conv to match the design.
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    UI_SCREEN_MAIN = 0,
    UI_SCREEN_PILL,
    UI_SCREEN_HUMIDITY,
    UI_SCREEN_COUNT
} ui_screen_t;

/*
 * Mounted orientation. The panel is 240x320 native but glued down rotated, so
 * the UI is 320x240 landscape with the buttons along the bottom. swap_xy is
 * always on; the mirror pair picks which of the four resulting orientations
 * you get, and only looking at the hardware settles it.
 */
/*
 * SETTLED on hardware 2026-08-31. With x=true,y=false the content was upright
 * only when the buttons were at the TOP, i.e. 180 degrees out from how the
 * board actually mounts. Toggling BOTH mirrors is that 180.
 */
#define UI_DEFAULT_MIRROR_X   false
#define UI_DEFAULT_MIRROR_Y   true

/* Requires display_init() to have succeeded first. */
esp_err_t ui_init(void);

/* Re-program the panel mirror live - no rebuild needed to try the other way. */
void ui_set_mirror(bool mirror_x, bool mirror_y);
void ui_get_mirror(bool *mirror_x, bool *mirror_y);

/* Move between the glance screens; wraps. */
void ui_next_screen(void);
void ui_prev_screen(void);
ui_screen_t ui_current_screen(void);
