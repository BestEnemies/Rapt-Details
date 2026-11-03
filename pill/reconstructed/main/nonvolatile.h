// nonvolatile.h — typed NVS key/value layer (reverse-engineered)
//
// Reconstructed from app_2.0.0.bin (source file: nonvolatile.cpp). A thin,
// type-safe, cached wrapper around ESP-IDF NVS. Confidence: High — the API
// shape is given verbatim by embedded assertion strings:
//   "T nonvolatile_get_impl(const NonVolatileKeyDef<T>*, bool* success)"
//   "(success != nullptr) || key->has_default"
//   "bool nonvolatile_set_string(NonVolatileKeysString, const char*)"
//   "char* nonvolatile_get_string(NonVolatileKeysString, char*, size_t)"
//
// All access is serialised by a single recursive mutex ("lock"); every key
// caches its last value so repeated reads avoid touching flash.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "nvs.h"

#ifdef __cplusplus
extern "C" {
#endif

// A key descriptor. The firmware keeps static arrays of these (one linked list
// per value type) so nonvolatile_erase() can invalidate every cache at once.
// Layout matches the decompiled struct (byte offsets in comments).
template <typename T>
struct NonVolatileKeyDef {
    const char* name;          // +0x00  NVS key (<=15 chars)
    T           default_value; // +0x04  value returned when the key is absent
    bool        has_default;   // +0x08  whether default_value is valid
    bool        cached_valid;  // +0x0a  cache holds a live value
    T           cached_value;  // +0x0c  last read/written value
    void*       next;          // +0x10  intrusive list link (for erase)
};

// Open the NVS namespace and create the lock. Returns false if NVS is
// unavailable. Called once from app_main via nonvolatile side effects.
bool nonvolatile_init(void);

// Erase the whole NVS partition (nvs_flash_erase) and invalidate every cached
// descriptor. Used by factory_reset / clear_registration.
bool nonvolatile_erase(void);

// Typed get/set. get() returns the cached/stored value, or the default when the
// key is absent; if success!=nullptr it is set to whether a stored value was
// actually found. set() writes through to flash and commits (skipping the write
// when the value is unchanged).
template <typename T> T    nonvolatile_get_impl(const NonVolatileKeyDef<T>* key, bool* success);
template <typename T> bool nonvolatile_set_impl(const NonVolatileKeyDef<T>* key, T value);

// String variants (NonVolatileKeysString is an index/descriptor into the
// string-key table). get copies into buf (size len) and returns buf, or NULL.
char* nonvolatile_get_string(const NonVolatileKeyDef<char*>* key, char* buf, size_t len);
bool  nonvolatile_set_string(const NonVolatileKeyDef<char*>* key, const char* value);

#ifdef __cplusplus
}
#endif
