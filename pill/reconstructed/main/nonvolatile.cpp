// nonvolatile.cpp — typed, cached NVS key/value layer (reverse-engineered)
//
// Reconstructed from app_2.0.0.bin. Confidence: High. See nonvolatile.h for the
// recovered API. SDK call map (resolved from the decompilation):
//   nvs_flash_init       FUN_0x40109eb0     nvs_open            FUN_0x40109f40
//   nvs_get_u32          FUN_0x4010a190     nvs_set_u32         FUN_0x4010a030
//   nvs_get_str          FUN_0x4010a1c4     nvs_set_str         FUN_0x4010a090
//   nvs_commit           FUN_0x4010a064     nvs_close           FUN_0x4010a2e8
//   nvs_flash_erase      FUN_0x4010a2a4     nvs_flash_deinit    FUN_0x4010a2d8
//   recursive-mutex take FUN_0x40091a98     give                FUN_0x400915a0
//   create rec. mutex    FUN_0x40091720     __assert_func       FUN_0x40098260
// ESP_ERR_NVS_NOT_FOUND = 0x1102, ESP_ERR_NVS_INVALID_LENGTH = 0x110c.

#include "nonvolatile.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char* TAG = "nvs";                 // PTR_DAT_400d22f8

// Module globals (DAT_400d2300 / 400d22f4 / 400d2308 / 400d230c).
static nvs_handle_t    s_nvs;                    // open handle         (*DAT_400d2300)
static bool            s_opened;                 // nvs_open succeeded   (*DAT_400d22f4)
static SemaphoreHandle_t s_lock;                 // recursive mutex      (*DAT_400d2308)
static StaticSemaphore_t s_lock_buf;             // static storage       (DAT_400d230c)

// Intrusive registries of every declared key, by storage type, so erase() can
// drop all caches. (Heads: DAT_400d16a8 / 400d172c / 400d1760 u32/float/str;
// DAT_400d2320 the bool keys, which use a smaller node.)
extern void* g_u32_keys;
extern void* g_float_keys;
extern void* g_str_keys;
extern void* g_bool_keys;

// nonvolatile_init @ nonvolatile.cpp:0x91 (FUN_0x400eb07c)
// Open NVS and lazily create the recursive lock.
bool nonvolatile_init(void)
{
    if (!s_opened) {
        esp_err_t err = nvs_flash_init();                 // FUN_0x40109eb0
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "nvs_flash_init() failed: %d", err);
            return false;
        }
        err = nvs_open(TAG, NVS_READWRITE, &s_nvs);       // FUN_0x40109f40 (mode 1)
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "nvs_open() failed: %d", err);
            nvs_flash_deinit();                           // FUN_0x4010a2d8
            return false;
        }
        s_opened = true;
    }
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateRecursiveMutexStatic(&s_lock_buf);  // FUN_0x40091720
        assert(s_lock != NULL /* "lock" */);              // nonvolatile.cpp:0x91
    }
    return true;
}

// Lazily create just the lock without opening NVS. (FUN_0x400eb024)
static bool ensure_lock(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateRecursiveMutexStatic(&s_lock_buf);
        return s_lock != NULL;
    }
    return true;
}

// nonvolatile_erase @ FUN_0x400eb104
// Flush+close the handle, erase the partition, invalidate every cached key,
// then re-init. Logs "Erasing NVS partition...".
bool nonvolatile_erase(void)
{
    if (s_lock) xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    if (s_opened) { nvs_close(s_nvs); s_opened = false; } // FUN_0x4010a2e8
    nvs_flash_deinit();                                   // FUN_0x4010a2d8
    ESP_LOGI(TAG, "Erasing NVS partition...");
    nvs_flash_erase();                                    // FUN_0x4010a2a4

    // Drop all cached values (cached_valid flag) across the key registries.
    for (auto* n = (NonVolatileKeyDef<uint32_t>*)g_u32_keys;   n; n = (NonVolatileKeyDef<uint32_t>*)n->next) n->cached_valid = false;
    for (auto* n = (NonVolatileKeyDef<float>*)   g_float_keys; n; n = (NonVolatileKeyDef<float>*)   n->next) n->cached_valid = false;
    for (auto* n = (NonVolatileKeyDef<char*>*)   g_str_keys;   n; n = (NonVolatileKeyDef<char*>*)   n->next) n->cached_valid = false;
    for (auto* n = (NonVolatileKeyDef<bool>*)    g_bool_keys;  n; n = (NonVolatileKeyDef<bool>*)    n->next) n->cached_valid = false; // smaller node

    bool ok = nonvolatile_init();                         // FUN_0x400eb07c (re-init)
    if (s_lock) xSemaphoreGiveRecursive(s_lock);
    return ok;
}

// nonvolatile_get_impl @ nonvolatile.cpp:0xcf (FUN_0x400eb1d8 u32 / FUN_0x400eb350 float)
// Return cached value if valid; else read from NVS, caching on success or on
// NOT_FOUND-with-default. *success (if given) = a stored value was found.
template <typename T>
T nonvolatile_get_impl(const NonVolatileKeyDef<T>* key, bool* success)
{
    assert(key != nullptr);
    assert((success != nullptr) || key->has_default);     // nonvolatile.cpp:0xd3

    if (key->cached_valid) {                               // byte +0x0a
        if (success) *success = true;
        return key->cached_value;
    }

    T value = key->default_value;
    if (s_lock == NULL || !s_opened) {                    // NVS unavailable
        if (success) *success = false;
        return value;
    }

    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    T tmp;
    esp_err_t err = nvs_get_u32(s_nvs, key->name, (uint32_t*)&tmp);  // FUN_0x4010a190
    bool found = (err == ESP_OK);
    if (err == ESP_OK || (err == ESP_ERR_NVS_NOT_FOUND && key->has_default)) {
        if (err != ESP_OK) tmp = value;                   // use default on NOT_FOUND
        const_cast<NonVolatileKeyDef<T>*>(key)->cached_value = tmp;
        const_cast<NonVolatileKeyDef<T>*>(key)->cached_valid = true;
        value = tmp;
        err = ESP_OK;
    }
    xSemaphoreGiveRecursive(s_lock);
    if (success) *success = (err == ESP_OK);
    return value;   // float variant memcpy's the u32 bits into a float first
}

// nonvolatile_set_impl @ nonvolatile.cpp:0x124 (FUN_0x400eb290 u32 / FUN_0x400eb418 float)
// Write-through with a skip when the stored value already matches.
template <typename T>
bool nonvolatile_set_impl(const NonVolatileKeyDef<T>* key, T value)
{
    if (!s_opened || s_lock == NULL) return false;
    assert(key != nullptr);
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);

    esp_err_t err;
    T current;
    if (nvs_get_u32(s_nvs, key->name, (uint32_t*)&current) == ESP_OK && current == value) {
        err = ESP_OK;                                     // unchanged: cache only
    } else {
        err = nvs_set_u32(s_nvs, key->name, *(uint32_t*)&value);  // FUN_0x4010a030
        if (err == ESP_OK) err = nvs_commit(s_nvs);               // FUN_0x4010a064
        if (err != ESP_OK)
            ESP_LOGE(TAG, "nvs_set_u32/commit failed for key %s: %d", key->name, err);
    }
    if (err == ESP_OK) {
        const_cast<NonVolatileKeyDef<T>*>(key)->cached_value = value;
        const_cast<NonVolatileKeyDef<T>*>(key)->cached_valid = true;
    }
    xSemaphoreGiveRecursive(s_lock);
    return err == ESP_OK;
}

// nonvolatile_get_string @ nonvolatile.cpp:0x209 (FUN_0x400eb508)
char* nonvolatile_get_string(const NonVolatileKeyDef<char*>* key, char* buf, size_t len)
{
    if (!s_opened || s_lock == NULL) return NULL;
    assert(key != nullptr);
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);

    size_t n = len;
    esp_err_t err = nvs_get_str(s_nvs, key->name, buf, &n);        // FUN_0x4010a1c4
    if (err == ESP_ERR_NVS_NOT_FOUND && key->has_default) {        // fall back to default
        const char* d = key->default_value;
        if (strlen(d) + 1 <= len) { strcpy(buf, d); err = ESP_OK; }
        else err = ESP_ERR_NVS_INVALID_LENGTH;                     // 0x110c
    }
    xSemaphoreGiveRecursive(s_lock);
    return (err == ESP_OK) ? buf : NULL;
}

// nonvolatile_set_string @ nonvolatile.cpp:0x275 (FUN_0x400eb63c)
// Reads the current string first; writes only when it differs (strcmp).
bool nonvolatile_set_string(const NonVolatileKeyDef<char*>* key, const char* value)
{
    if (!s_opened || s_lock == NULL) return false;
    assert(key != nullptr);
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);

    size_t need = strlen(value) + 1;
    // Small strings (<=512 B) use a pooled/stack buffer; larger ones malloc.
    char* tmp = (need <= 512) ? (char*)alloca(need) : (char*)malloc(need);  // FUN_0x40081b5c/40098368

    size_t n = need;
    esp_err_t err = nvs_get_str(s_nvs, key->name, tmp, &n);
    bool changed = (err != ESP_OK) || (strcmp(tmp, value) != 0);   // DAT_400d02e8 = strcmp
    if (changed) {
        err = nvs_set_str(s_nvs, key->name, value);                // FUN_0x4010a090
        if (err == ESP_OK) err = nvs_commit(s_nvs);
        if (err != ESP_OK)
            ESP_LOGE(TAG, "nvs_set_str/commit failed for key %s: %d", key->name, err);
    } else {
        err = ESP_OK;
    }
    if (need > 512) free(tmp);                                     // FUN_0x40098388
    xSemaphoreGiveRecursive(s_lock);
    return err == ESP_OK;
}

// =============================================================================
// NVS string-dyn get + typed delete (extension). Recovered source "nonvolatile.cpp".
// All serialised by the module mutex (FUN_0x40091a98 take / FUN_0x400915a0 give).
// Addresses are Ghidra FUN_<addr> symbols. Confidence: HIGH (assert __func__ +
// source line numbers embedded: nonvolatile.cpp:0x239 / :0x180).
// =============================================================================

// FUN_0x400eb590 - char *nonvolatile_get_string_dyn(key): read a string value,
// malloc'ing the exact buffer. If the key is missing (ESP_ERR_NVS_NOT_FOUND =
// 0x1102) and the key has a default (key->has_default), returns a malloc'd copy
// of the default; otherwise NULL. Caller frees. (nonvolatile.cpp:0x239)
char *nonvolatile_get_string_dyn(const NonVolatileKeyDef<const char*> *key)
{
    if (!s_nvs || !s_opened) return nullptr;
    assert(key != nullptr);                               // "key != nullptr"
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);                                            // FUN_0x40091a98
    char *out = nullptr;
    size_t len = 0;
    esp_err_t e = nvs_get_str(s_nvs, key->name, nullptr, &len); // FUN_0x4010a1c4
    if (e == ESP_OK) {
        out = (char *)malloc(len);                        // FUN_0x40098368
        if (out && nvs_get_str(s_nvs, key->name, out, &len) != ESP_OK) {
            free(out); out = nullptr;
        }
    } else if (e == ESP_ERR_NVS_NOT_FOUND && key->has_default) {   // 0x1102
        const char *d = key->default_value;
        out = (char *)malloc(strlen(d) + 1);
        if (out) strcpy(out, d);
    }
    xSemaphoreGiveRecursive(s_lock);                                         // FUN_0x400915a0
    return out;
}

// FUN_0x400eb824 / 0x400eb8f8 / 0x400eb9cc - bool nonvolatile_delete_impl(key):
// three type-specialisations of the template (same shape, different key
// registry). Erase the key, clear its cache bit, and commit. Treats
// ESP_ERR_NVS_NOT_FOUND as success. Logs "nvs_erase_key failed" /
// "nvs_commit failed" on real errors. (nonvolatile.cpp:0x180)
template <typename T>
bool nonvolatile_delete_impl(NonVolatileKeyDef<T> *key)
{
    if (!s_nvs || !s_opened) return false;
    assert(key != nullptr);                               // "key != nullptr"
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    esp_err_t e = nvs_erase_key(s_nvs, key->name); // FUN_0x40109f58
    esp_err_t result = ESP_OK;
    if (e == ESP_OK || e == ESP_ERR_NVS_NOT_FOUND) {
        key->cached_valid = false;                        // *(key+10) = 0
        if (e == ESP_OK && (result = nvs_commit(s_nvs)) != ESP_OK) // FUN_0x4010a064
            ESP_LOGE(TAG, "nvs_commit failed in nonvolatile_delete: %d", result);
    } else {
        ESP_LOGE(TAG, "nvs_erase_key failed for %s: %d", key->name, e);
        result = e;
    }
    xSemaphoreGiveRecursive(s_lock);
    return result == ESP_OK || result == ESP_ERR_NVS_NOT_FOUND;
}
// explicit instantiations present in the binary (bool/u32, string, float/blob):
template bool nonvolatile_delete_impl<uint32_t>(NonVolatileKeyDef<uint32_t>*);
template bool nonvolatile_delete_impl<const char*>(NonVolatileKeyDef<const char*>*);
template bool nonvolatile_delete_impl<float>(NonVolatileKeyDef<float>*);
