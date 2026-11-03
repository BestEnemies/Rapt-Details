#include "display.h"
#include "board.h"

#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_vendor.h"
#include "driver/spi_master.h"

static const char *TAG = "display";

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t    s_panel;

esp_err_t display_init(void)
{
    /*
     * Backlight first, so a panel that fails to initialise still shows light
     * rather than looking dead - that distinction cost us a debugging round.
     */
    gpio_config_t bl = {
        .pin_bit_mask = (1ULL << BOARD_LCD_BL),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&bl);
    gpio_set_level(BOARD_LCD_BL, 1);

    spi_bus_config_t bus = {
        .mosi_io_num     = BOARD_LCD_MOSI,
        .miso_io_num     = BOARD_LCD_MISO,
        .sclk_io_num     = BOARD_LCD_SCLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = BOARD_LCD_MAX_TRANSFER,
    };
    esp_err_t err = spi_bus_initialize(BOARD_LCD_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(err));
        return err;
    }

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num       = BOARD_LCD_CS,
        .dc_gpio_num       = BOARD_LCD_DC,
        .spi_mode          = 0,
        .pclk_hz           = BOARD_LCD_CLOCK_HZ,
        .trans_queue_depth = 8,
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
    };
    err = esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)BOARD_LCD_HOST, &io_cfg, &s_io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel_io_spi: %s", esp_err_to_name(err));
        return err;
    }

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = BOARD_LCD_RST,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    err = esp_lcd_new_panel_st7789(s_io, &panel_cfg, &s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "new_panel_st7789: %s", esp_err_to_name(err));
        return err;
    }

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    ESP_LOGI(TAG, "init: ST7789V %dx%d, MOSI=%d SCLK=%d CS=%d DC=%d RST=%d BL=%d",
             BOARD_LCD_H_RES, BOARD_LCD_V_RES,
             BOARD_LCD_MOSI, BOARD_LCD_SCLK, BOARD_LCD_CS,
             BOARD_LCD_DC, BOARD_LCD_RST, BOARD_LCD_BL);
    return ESP_OK;
}

esp_lcd_panel_handle_t display_panel(void)
{
    return s_panel;
}

esp_lcd_panel_io_handle_t display_io(void)
{
    return s_io;
}

esp_err_t display_fill(uint16_t rgb565)
{
    if (!s_panel) {
        return ESP_ERR_INVALID_STATE;
    }

    /* One row at a time keeps us well inside max_transfer_sz. */
    const int row_bytes = BOARD_LCD_H_RES * sizeof(uint16_t);
    uint16_t *row = heap_caps_malloc(row_bytes, MALLOC_CAP_DMA);
    if (!row) {
        return ESP_ERR_NO_MEM;
    }

    /*
     * The panel consumes RGB565 most-significant byte first; the ESP32 is
     * little-endian, so a uint16_t goes out with its bytes reversed. Verified
     * on hardware: filling with 0x001F (blue) unswapped rendered as 0x1F00,
     * which is green.
     *
     * LVGL solves the same problem with LV_COLOR_16_SWAP; here we swap once
     * per fill, which costs nothing.
     */
    const uint16_t be = (uint16_t)((rgb565 >> 8) | (rgb565 << 8));
    for (int x = 0; x < BOARD_LCD_H_RES; x++) {
        row[x] = be;
    }

    esp_err_t err = ESP_OK;
    for (int y = 0; y < BOARD_LCD_V_RES; y++) {
        err = esp_lcd_panel_draw_bitmap(s_panel, 0, y, BOARD_LCD_H_RES, y + 1, row);
        if (err != ESP_OK) {
            break;
        }
    }
    heap_caps_free(row);
    return err;
}

esp_err_t display_probe_id(uint8_t *out, size_t len)
{
    if (!s_io || !out) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 0x04 = RDDID on both ILI9341 and ST7789. */
    return esp_lcd_panel_io_rx_param(s_io, 0x04, out, len);
}
