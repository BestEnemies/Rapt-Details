/*
 * board.h - hardware map for the KegLand RAPT FridgeG5 controller.
 *
 * Every assignment below was recovered from the stock `fancyfridge` firmware
 * by enumerating each gpio_set_level / gpio_get_level / ADC / SPI call site
 * and resolving its constant argument. 26 output sites and 25 input sites
 * were found, all with constant arguments, so this is the complete inventory
 * rather than a sample. The recovery-mode diagnostic handlers and the main
 * application agree on every pin.
 *
 * CONFIDENCE MARKERS
 *   [exact]      read directly from a constant in the instruction stream
 *   [unverified] inferred; confirm on hardware before trusting
 */

#pragma once

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "hal/adc_types.h"

/* -------------------------------------------------------------------------
 * Outputs - all active-high through relay drivers.
 * ---------------------------------------------------------------------- */
#define BOARD_GPIO_HEATER       GPIO_NUM_18   /* [exact] heating element   */
#define BOARD_GPIO_COOLER       GPIO_NUM_33   /* [exact] compressor        */
#define BOARD_GPIO_FAN          GPIO_NUM_19   /* [exact] circulation fan   */
#define BOARD_GPIO_LIGHT        GPIO_NUM_21   /* [exact] chamber light     */
#define BOARD_GPIO_BUZZER       GPIO_NUM_32   /* [exact] audible alerts    */

/* -------------------------------------------------------------------------
 * Inputs.
 *
 * GPIO 35/36/39 are input-only on the ESP32 and have NO internal pull-ups,
 * so the board must provide external ones. GPIO 15 is also a boot strapping
 * pin - holding it low at reset silences the ROM boot log, which is harmless
 * but worth knowing when a button appears stuck.
 *
 * The stock firmware's input-test screen prints seven columns
 * (up down select back pump door play) but reads only five pins; `pump` and
 * `play` are passed as a hardcoded 1, i.e. not fitted on this unit.
 * ---------------------------------------------------------------------- */
#define BOARD_GPIO_BTN_UP       GPIO_NUM_15   /* [exact] */
#define BOARD_GPIO_BTN_DOWN     GPIO_NUM_13   /* [exact] */
#define BOARD_GPIO_BTN_SELECT   GPIO_NUM_39   /* [exact] input-only */
#define BOARD_GPIO_BTN_BACK     GPIO_NUM_36   /* [exact] input-only */
#define BOARD_GPIO_DOOR         GPIO_NUM_35   /* [exact] input-only */

/*
 * VERIFIED on hardware 2026-08-31: all four buttons read inactive at rest and
 * active when pressed, with clean single edges. Active-low confirmed.
 *
 * (The disassembly could not settle this - the stock firmware prints the raw
 * pin state without inverting it. It took a press test on the real board.)
 */
#define BOARD_BUTTON_ACTIVE_LEVEL   0

/* -------------------------------------------------------------------------
 * Temperature probe.
 *
 * Exactly one ADC channel is read anywhere in the stock firmware - recovery
 * mode and main application alike. The second column on the stock ADC test
 * screen is never populated.
 * ---------------------------------------------------------------------- */
#define BOARD_ADC_UNIT          ADC_UNIT_1
#define BOARD_ADC_CHANNEL       ADC_CHANNEL_6    /* [exact] = GPIO 34 */
#define BOARD_ADC_ATTEN         ADC_ATTEN_DB_12  /* [exact] stock uses 11 dB;
                                                    renamed DB_12 in IDF v5 */
#define BOARD_ADC_BITWIDTH      ADC_BITWIDTH_12  /* [exact] */

/*
 * Divider constants, read directly out of the stock conversion routine at
 * 0x40107e65..0x40107e83:
 *
 *   l32r  3300.0 ; sub.s (3300 - V) ; l32r 56000.0 ; mul.s (V * 56000) ; divide
 *
 * i.e. R = 56000 * V / (3300 - V), thermistor on the LOW side.
 *
 * (An earlier revision of this file used 10000.0 here. That constant does sit
 * in the same literal pool but belongs to unrelated code - the series resistor
 * is 56 kohm.)
 */
#define BOARD_PROBE_SERIES_OHMS     56000.0f     /* [exact] */
#define BOARD_PROBE_SUPPLY_MV       3300.0f      /* [exact] */

/*
 * Thermistor model. The stock firmware reads these at runtime via
 * nonvolatile_get_float() as NVS keys ntc_beta0 / ntc_t0 / ntc_r0 (float key
 * indices 5, 6, 7), falling back to the defaults table at 0x3f40e0c4.
 *
 * These ARE those defaults. A production unit may carry per-unit overrides in
 * NVS, plus a two-point ADC correction (cal_temp0/cal_adc0, cal_temp1/cal_adc1)
 * and a final temp_offset, none of which are implemented here yet.
 */
#define BOARD_NTC_BETA              3950.0f      /* [exact] ntc_beta0 default */
#define BOARD_NTC_T0_KELVIN         298.15f      /* [exact] ntc_t0    default */
#define BOARD_NTC_R0_OHMS           100000.0f    /* [exact] ntc_r0    default */

/*
 * Stock firmware samples 128 times and takes sum >> 7 as the mean, and
 * invalidates the whole reading if any raw sample escapes this range -
 * that is open-circuit / short-circuit probe detection. Replicated exactly.
 */
#define BOARD_PROBE_SAMPLES         128          /* [exact] */
#define BOARD_PROBE_RAW_MIN         2            /* [exact] */
#define BOARD_PROBE_RAW_MAX         4093         /* [exact] */

/* -------------------------------------------------------------------------
 * Display - ST7789V on SPI.
 *
 * MOSI, SCLK and CS are exact: read from the spi_bus_config_t and
 * spi_device_interface_config_t structs built on the stack at 0x401844a4.
 *
 * IMPORTANT: the stock component (`lvgl_ili9341`) contains spi_display_probe.c
 * and auto-detects the panel, which means more than one controller variant
 * ships across the product line. Do not assume ST7789V on hardware you have
 * not checked.
 * ---------------------------------------------------------------------- */
#define BOARD_LCD_HOST          SPI2_HOST      /* [exact] host 1 = HSPI */
#define BOARD_LCD_MOSI          GPIO_NUM_23    /* [exact] */
#define BOARD_LCD_SCLK          GPIO_NUM_22    /* [exact] */
#define BOARD_LCD_MISO          (-1)           /* [exact] write-only */
#define BOARD_LCD_CS            GPIO_NUM_5     /* [exact] spics_io_num */
#define BOARD_LCD_CLOCK_HZ      (10 * 1000 * 1000) /* [exact] 20 MHz variant exists */
#define BOARD_LCD_MAX_TRANSFER  0x2800         /* [exact] 10240 bytes */

/*
 * [exact] All three recovered from the stock driver.
 *
 * RST = GPIO 4: probe_spi_init at 0x40184c8c pulses it high, 20 ms, low,
 * 20 ms, high - an unmistakable hardware reset sequence.
 *
 * BL  = GPIO 27: gpio_set_level(27) sits beside the "Enabling"/"Disabling"
 * and "%s backlight." strings at 0x3f451102.
 *
 * DC  = GPIO 2: the SPI pre-transfer callback at 0x40090660 does
 *     entry a1,32 ; l32i.n a11,a2,24 ; movi.n a10,2 ; callx8 -> gpio_set_level
 * i.e. gpio_set_level(2, transaction->user). It went unnoticed at first
 * because the call is an indirect callx8 rather than a direct call8, so it
 * did not appear in a scan of gpio_set_level call sites.
 *
 * GPIO 2 is a boot strapping pin. It must not be held high at reset, but
 * driving it as an output after boot is fine - which is what the stock
 * firmware does.
 */
#define BOARD_LCD_DC            GPIO_NUM_2     /* [exact] */
#define BOARD_LCD_RST           GPIO_NUM_4     /* [exact] */
#define BOARD_LCD_BL            GPIO_NUM_27    /* [exact] active high */

/*
 * Panel geometry. The ST7789 is natively 240x320 portrait, but this board
 * MOUNTS IT ROTATED: with the buttons along the bottom edge, the usable
 * display is 320 wide by 240 tall.
 *
 * NATIVE_* is what the controller expects; UI_* is what anything drawing on
 * it should use. The rotation is applied in the LVGL port, not by rotating
 * coordinates in the UI code.
 */
#define BOARD_LCD_NATIVE_H_RES  240
#define BOARD_LCD_NATIVE_V_RES  320

#define BOARD_LCD_UI_W          320
#define BOARD_LCD_UI_H          240

/* Kept for the bring-up fill, which addresses the panel natively. */
#define BOARD_LCD_H_RES         BOARD_LCD_NATIVE_H_RES
#define BOARD_LCD_V_RES         BOARD_LCD_NATIVE_V_RES

/* -------------------------------------------------------------------------
 * Spare expansion header: 3V3 / IO25 / IO26 / GND
 *
 * Confirmed FREE. An exhaustive scan of every gpio_set_level, gpio_get_level
 * and gpio_set_direction call site - including indirect calls through a
 * function pointer, which is how the DC line on GPIO 2 hid from an earlier
 * pass - finds no reference to either pin in the stock firmware. Nor is any
 * DAC, LEDC, MCPWM, touch, ADC2, RMT, I2S or PCNT code linked in.
 *
 * Useful properties:
 *   - neither is a strapping pin, so they have no effect on boot
 *   - both are RTC GPIOs, so both can wake the chip from deep sleep
 *   - GPIO 25 is DAC1 and GPIO 26 is DAC2 - the ESP32's only true analogue
 *     outputs (8-bit)
 *
 * Caveat: both are also ADC2 channels (8 and 9), and ADC2 cannot be used
 * while WiFi is running. Treat them as digital or DAC pins, not ADC inputs.
 *
 * Good candidates for a fermentation chamber: 1-Wire (a DS18B20 chain is
 * factory-calibrated, which sidesteps the NTC calibration problem entirely),
 * I2C for extra sensors, or a second SSR output.
 * ---------------------------------------------------------------------- */
#define BOARD_SPARE_IO_A        GPIO_NUM_25    /* also DAC1 */
#define BOARD_SPARE_IO_B        GPIO_NUM_26    /* also DAC2 */

/* I2C on the spare header, for the humidity sensor (and anything else). */
#define BOARD_I2C_PORT          I2C_NUM_0
#define BOARD_I2C_SDA           BOARD_SPARE_IO_A
#define BOARD_I2C_SCL           BOARD_SPARE_IO_B
#define BOARD_I2C_HZ            100000
