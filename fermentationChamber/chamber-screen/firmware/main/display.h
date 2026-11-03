/*
 * display.h - ST7789V panel on SPI2.
 *
 * MOSI, SCLK, CS, clock rate and max transfer size are exact, read from the
 * stock firmware's spi_bus_config_t and spi_device_interface_config_t.
 *
 * The DC and RST assignments are NOT confirmed - GPIO 4 and 27 are both only
 * ever touched inside the stock display driver, but which is which was not
 * separated. If the panel stays dark, swapping BOARD_LCD_DC and
 * BOARD_LCD_RST in board.h is the first thing to try.
 *
 * Also note the stock component contains spi_display_probe.c and auto-detects
 * the controller, so more than one panel variant ships in this product line.
 * display_probe_id() is provided as a hook for the same approach.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

esp_err_t display_init(void);

/* Panel handle, for wiring up LVGL or drawing directly. */
esp_lcd_panel_handle_t display_panel(void);

/* Panel IO handle - esp_lvgl_port needs both. */
esp_lcd_panel_io_handle_t display_io(void);

/* Fill the whole panel with one RGB565 colour - useful as a first bring-up test. */
esp_err_t display_fill(uint16_t rgb565);

/*
 * Read the controller's ID registers. The stock firmware probes rather than
 * assuming; do the same before shipping to hardware you have not opened.
 * Returns the raw ID bytes read; interpretation is controller-specific.
 */
esp_err_t display_probe_id(uint8_t *out, size_t len);
