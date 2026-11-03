#include "humidity.h"
#include "board.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "humidity";

#define SHT_ADDR_PRIMARY    0x44
#define SHT_ADDR_SECONDARY  0x45
#define POLL_INTERVAL_MS    5000

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static humidity_sensor_t       s_type;
static humidity_reading_t      s_last;
static SemaphoreHandle_t       s_lock;

/* Sensirion CRC8: polynomial 0x31, init 0xFF, no reflection, no final xor. */
static uint8_t sensirion_crc(const uint8_t *d, int len)
{
    uint8_t crc = 0xFF;
    for (int i = 0; i < len; i++) {
        crc ^= d[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static bool frame_ok(const uint8_t *f)
{
    return sensirion_crc(&f[0], 2) == f[2] &&
           sensirion_crc(&f[3], 2) == f[5];
}

static esp_err_t cmd8(uint8_t c)
{
    return i2c_master_transmit(s_dev, &c, 1, 100);
}

static esp_err_t cmd16(uint16_t c)
{
    uint8_t b[2] = { (uint8_t)(c >> 8), (uint8_t)(c & 0xFF) };
    return i2c_master_transmit(s_dev, b, sizeof(b), 100);
}

/* ------------------------------------------------------------------ */

static esp_err_t attach(uint8_t addr)
{
    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = BOARD_I2C_HZ,
    };
    return i2c_master_bus_add_device(s_bus, &dc, &s_dev);
}

static bool probe_sht4x(void)
{
    /* 0x89 = read serial number. Only the SHT4x answers it. */
    if (cmd8(0x89) != ESP_OK) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    uint8_t f[6] = { 0 };
    if (i2c_master_receive(s_dev, f, sizeof(f), 100) != ESP_OK) {
        return false;
    }
    return frame_ok(f);
}

static bool probe_sht3x(void)
{
    /* 0xF32D = read status register. */
    if (cmd16(0xF32D) != ESP_OK) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    uint8_t f[3] = { 0 };
    if (i2c_master_receive(s_dev, f, sizeof(f), 100) != ESP_OK) {
        return false;
    }
    return sensirion_crc(f, 2) == f[2];
}

static bool read_once(float *rh, float *tc)
{
    uint8_t f[6] = { 0 };

    if (s_type == HUM_SENSOR_SHT4X) {
        if (cmd8(0xFD) != ESP_OK) {          /* high precision measurement */
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(12));
    } else {
        if (cmd16(0x2400) != ESP_OK) {       /* single shot, high rep, no stretch */
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    if (i2c_master_receive(s_dev, f, sizeof(f), 100) != ESP_OK) {
        return false;
    }
    if (!frame_ok(f)) {
        ESP_LOGW(TAG, "CRC mismatch; discarding reading");
        return false;
    }

    uint16_t rt = (uint16_t)((f[0] << 8) | f[1]);
    uint16_t rr = (uint16_t)((f[3] << 8) | f[4]);

    *tc = -45.0f + 175.0f * (float)rt / 65535.0f;

    if (s_type == HUM_SENSOR_SHT4X) {
        *rh = -6.0f + 125.0f * (float)rr / 65535.0f;
    } else {
        *rh = 100.0f * (float)rr / 65535.0f;
    }

    /* The SHT4x transfer function can produce slightly out-of-range values. */
    if (*rh < 0.0f)   *rh = 0.0f;
    if (*rh > 100.0f) *rh = 100.0f;
    return true;
}

static void poll_task(void *arg)
{
    (void)arg;
    int fails = 0;
    for (;;) {
        float rh, tc;
        if (read_once(&rh, &tc)) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_last.valid   = true;
            s_last.rh      = rh;
            s_last.celsius = tc;
            s_last.last_us = esp_timer_get_time();
            xSemaphoreGive(s_lock);
            fails = 0;
        } else if (++fails == 3) {
            /*
             * Do not clear the last good reading on a single hiccup, but do
             * stop claiming freshness - the consumer checks the age.
             */
            ESP_LOGW(TAG, "3 consecutive read failures");
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

/* ------------------------------------------------------------------ */

esp_err_t humidity_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }

    i2c_master_bus_config_t bc = {
        .i2c_port          = BOARD_I2C_PORT,
        .sda_io_num        = BOARD_I2C_SDA,
        .scl_io_num        = BOARD_I2C_SCL,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bc, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus: %s", esp_err_to_name(err));
        return err;
    }

    const uint8_t addrs[] = { SHT_ADDR_PRIMARY, SHT_ADDR_SECONDARY };
    for (int i = 0; i < 2; i++) {
        if (i2c_master_probe(s_bus, addrs[i], 100) != ESP_OK) {
            continue;
        }
        if (attach(addrs[i]) != ESP_OK) {
            continue;
        }
        if (probe_sht4x()) {
            s_type = HUM_SENSOR_SHT4X;
        } else if (probe_sht3x()) {
            s_type = HUM_SENSOR_SHT3X;
        } else {
            i2c_master_bus_rm_device(s_dev);
            s_dev = NULL;
            continue;
        }
        ESP_LOGI(TAG, "found %s at 0x%02x (SDA=%d SCL=%d)",
                 humidity_sensor_name(), addrs[i],
                 BOARD_I2C_SDA, BOARD_I2C_SCL);
        if (xTaskCreate(poll_task, "humidity", 3072, NULL, 4, NULL) != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
        return ESP_OK;
    }

    ESP_LOGW(TAG, "no SHT3x/SHT4x found on the spare header");
    return ESP_ERR_NOT_FOUND;
}

humidity_sensor_t humidity_sensor_type(void)
{
    return s_type;
}

const char *humidity_sensor_name(void)
{
    switch (s_type) {
    case HUM_SENSOR_SHT3X: return "SHT3x";
    case HUM_SENSOR_SHT4X: return "SHT4x";
    default:               return "none";
    }
}

bool humidity_get(humidity_reading_t *out)
{
    if (!out || !s_lock || s_type == HUM_SENSOR_NONE) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_last;
    xSemaphoreGive(s_lock);
    return out->valid;
}

int humidity_age_seconds(void)
{
    humidity_reading_t r;
    if (!humidity_get(&r)) {
        return -1;
    }
    return (int)((esp_timer_get_time() - r.last_us) / 1000000);
}
