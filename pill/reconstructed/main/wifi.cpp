// wifi.cpp — RAPT Pill 2.0.0, reverse-engineered (readable reconstruction)
//
// RAPT glue over esp32-wifi-manager. Confidence: Medium.
// The code faithfully follows the decompiled control flow; the FreeRTOS/NVS
// callees were identified from their signatures. Globals that hold the
// library's handles are shown as g_wm.* with a note where the exact field is
// inferred.
//
// Source addresses (Ghidra) are given per function.
#include "wifi.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "wifi_manager";

// ---- external state owned by the esp32-wifi-manager library -----------------
// Reconstructed from the DAT_ globals the functions touch. Field names are
// inferred; what matters is the layout the code assumes.
struct wifi_manager_state {
    wifi_config_t *sta_config;   // DAT_400d2120 : malloc'd 0xB8 wifi_config_t
    SemaphoreHandle_t json_mutex;// DAT_400d22f0 : guards sta_config
    QueueHandle_t    queue;      // DAT_400d2188 : manager message queue
    SemaphoreHandle_t event_loop_mutex; // DAT_400d2194
    // ... plus many handles freed in wifi_manager_destroy() (DAT_400d2168 etc.)
};
extern wifi_manager_state g_wm; /* inferred aggregate of the library globals */

// =============================================================================
// wifi_manager_lock_json_buffer @ 0x400eb044
// Take the config/JSON mutex with a tick timeout. Returns true if acquired.
bool wifi_manager_lock_json_buffer(TickType_t ticks)
{
    if (g_wm.json_mutex == nullptr)
        return false;
    return xSemaphoreTake(g_wm.json_mutex, ticks) == pdTRUE;
}

// wifi_manager_unlock_json_buffer @ 0x400eb064
void wifi_manager_unlock_json_buffer(void)
{
    xSemaphoreGive(g_wm.json_mutex);
}

// =============================================================================
// wifi_manager_send_message @ 0x400ea028
// Enqueue a {code, param} message for the manager task.
BaseType_t wifi_manager_send_message(int code, void *param)
{
    if (g_wm.queue == nullptr) {
        ESP_LOGW(TAG, "wifi_manager_send_message: queue not ready"); /* fmt DAT_400d21c4 */
        return pdFALSE;
    }
    struct { int code; void *param; } msg = { code, param };
    return xQueueSend(g_wm.queue, &msg, portMAX_DELAY);
}

// wifi_manager_stop_ap @ 0x400eaec4
void wifi_manager_stop_ap(void)
{
    ESP_LOGI(TAG, "Stopping access point");
    wifi_manager_send_message(WM_ORDER_STOP_AP, nullptr);
}

// =============================================================================
// wifi_manager_save_sta_config @ 0x400e9890
// RAPT copy of the library routine: writes the in-RAM wifi_config_t SSID and
// password blobs to NVS, but only the fields that actually changed.
void wifi_manager_save_sta_config(void)
{
    uint8_t ssid[32];
    uint8_t password[64];
    memset(ssid, 0, sizeof ssid);          // (buffer is 0xB8 total in the original)
    memset(password, 0, sizeof password);

    ESP_LOGI(TAG, "About to save config to flash!!");

    // Need a config object and the config lock.
    if (g_wm.sta_config == nullptr || !wifi_manager_lock_json_buffer(portMAX_DELAY)) {
        ESP_LOGE(TAG, "wifi_manager_save_sta_config failed to acquire ...");
        return;
    }

    nvs_handle_t h;
    if (nvs_open(WIFIMGR_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        wifi_manager_unlock_json_buffer();
        return;
    }

    bool changed = false;

    // --- SSID ---------------------------------------------------------------
    size_t len = sizeof ssid;
    esp_err_t e = nvs_get_blob(h, WIFIMGR_NVS_KEY_SSID, ssid, &len);
    bool missing_or_present = (e == ESP_OK) || (e == ESP_ERR_NVS_NOT_FOUND);
    if (missing_or_present) {
        if (memcmp(ssid, g_wm.sta_config->sta.ssid, sizeof ssid) != 0) {
            if (nvs_set_blob(h, WIFIMGR_NVS_KEY_SSID, g_wm.sta_config->sta.ssid, 32) == ESP_OK) {
                ESP_LOGI(TAG, "wifi_manager_wrote wifi_sta_config: ssid");
                changed = true;
            }
        }
    }

    // --- password -----------------------------------------------------------
    len = sizeof password;
    e = nvs_get_blob(h, WIFIMGR_NVS_KEY_PASS, password, &len);
    if (e == ESP_OK || e == ESP_ERR_NVS_NOT_FOUND) {
        if (memcmp(password, g_wm.sta_config->sta.password, sizeof password) != 0) {
            if (nvs_set_blob(h, WIFIMGR_NVS_KEY_PASS, g_wm.sta_config->sta.password, 64) == ESP_OK) {
                ESP_LOGI(TAG, "wifi_manager_wrote wifi_sta_config: password");
                changed = true;
            }
        } else if (!changed) {
            ESP_LOGI(TAG, "Wifi config was not saved to flash because no change ...");
        }
    }

    if (changed)
        nvs_commit(h);
    nvs_close(h);
    wifi_manager_unlock_json_buffer();
}

// =============================================================================
// wifi_manager_fetch_wifi_sta_config @ 0x400e9a60
// Load the SSID/password blobs from NVS into a freshly-allocated wifi_config_t.
// Returns true if a non-empty SSID was loaded.
bool wifi_manager_fetch_wifi_sta_config(void)
{
    if (!wifi_manager_lock_json_buffer(portMAX_DELAY))
        return false;

    nvs_handle_t h;
    if (nvs_open(WIFIMGR_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        wifi_manager_unlock_json_buffer();
        return false;
    }

    if (g_wm.sta_config == nullptr)
        g_wm.sta_config = (wifi_config_t *)malloc(sizeof(wifi_config_t)); // 0xB8
    memset(g_wm.sta_config, 0, sizeof(wifi_config_t));

    // Scratch buffer (0x6C) for the blob reads.
    void *scratch = malloc(0x6C);
    memset(scratch, 0, 4);

    size_t len = 32;
    bool ok = false;
    if (nvs_get_blob(h, WIFIMGR_NVS_KEY_SSID, scratch, &len) == ESP_OK) {
        memcpy(g_wm.sta_config->sta.ssid, scratch, len);
        len = 64;
        if (nvs_get_blob(h, WIFIMGR_NVS_KEY_PASS, scratch, &len) == ESP_OK) {
            memcpy(g_wm.sta_config->sta.password, scratch, len);
            ESP_LOGI(TAG, "wifi_manager_fetch_wifi_sta_config");
            ok = (g_wm.sta_config->sta.ssid[0] != '\0');
        }
    }
    free(scratch);
    nvs_close(h);
    wifi_manager_unlock_json_buffer();
    return ok;
}

// =============================================================================
// wifi_manager_destroy @ 0x400ea3b8
// Full teardown: gracefully stop the event loop, then free every handle and
// the per-bucket SSID scan lists, delete semaphores/queues/event group and
// run the netif cleanup. Confidence: Medium (frees follow the decompiled order;
// individual DAT_ handles are not all named).
void wifi_manager_destroy(void)
{
    wifi_manager_send_message(WM_ORDER_START_WIFI_SCAN, nullptr); // 0x0e /* inferred */

    if (xSemaphoreTake(g_wm.event_loop_mutex, 200) == pdTRUE) {
        xSemaphoreGive(g_wm.event_loop_mutex);
        ESP_LOGI(TAG, "wifi_manager_event_loop_mutex released");
    } else {
        ESP_LOGW(TAG, "could not gracefully end ...");
        // vEventGroupDelete(wifi_manager_event_group); *handle = 0;
    }

    // free(wifi_manager_json_*); free scan accesspoint lists[16]; free sta_config;
    // vSemaphoreDelete(...); vQueueDelete(queue); vEventGroupDelete(...);
    // esp_event_handler_unregister(...) x3; esp_wifi/netif cleanup.
    // (Handle-by-handle; omitted here — see decompilation. All set to NULL.)

    ESP_LOGI(TAG, "got to end of wifi manager");
}
