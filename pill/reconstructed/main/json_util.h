// json_util.h — recovered JSON accessor facade (reverse-engineered)
//
// The firmware uses a small JSON wrapper reached through pointer tables in the
// decompilation (PTR_FUN_400d08c4 = type, PTR_FUN_400d096c = root value, etc.)
// and a set of app helpers at 0x400e86xx–0x400e95xx. It is dotted-path aware
// ("state.reported" creates/looks up nested objects), which points to a thin
// custom layer over a parser (very likely cJSON). Names below are reconstructed;
// signatures reflect observed call sites. Confidence: MEDIUM (facade), the
// call *sites* are HIGH.
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct js_value js_value;

enum { JS_NULL=0, JS_BOOL=1, JS_NUMBER=3, JS_OBJECT=4, JS_ARRAY=5, JS_STRING=2 }; /* codes inferred: 4==object (OTA/shadow checks), 3==number */

// --- parse / lifecycle ---
js_value  *js_parse(const char *text);                 // FUN_400e91f8 (BOM tolerant)
js_value  *js_root(js_value *parsed);                  // PTR_FUN_400d096c
int        js_type(js_value *v);                       // PTR_FUN_400d08c4
void       js_free(js_value *parsed);                  // FUN_400e8a70 / FUN_400e93c4
char      *js_serialise(js_value *v);                  // FUN_400e93d4 (-> heap string)

// --- create ---
js_value  *js_new_object(void);                        // FUN_400e8bcc

// --- typed getters (by key, on an object) ---
const char *js_get_string(js_value *obj, const char *key);  // FUN_400e871c
int         js_get_bool  (js_value *obj, const char *key);  // FUN_400e877c (1/0/-1)
double      js_get_number(js_value *obj, const char *key);  // FUN_400e8744
js_value   *js_get_object(js_value *obj, const char *key);  // FUN_400e8670
js_value   *js_get_member(js_value *obj, const char *dotted_path); // FUN_400e86c0/862c

// --- builders (return 0 on success in the binary) ---
int js_add_string (js_value *obj, const char *key, const char *val);        // FUN_400e94d4
int js_add_number (js_value *obj, const char *key, double val);             // FUN_400e9458
int js_add_i64    (js_value *obj, const char *key, int64_t val);            // FUN_400e94f8
int js_add_object (js_value *root, const char *dotted_path, js_value *child);// FUN_400e9560
js_value *js_array_in(js_value *obj, const char *key);                      // FUN_400e9458(array)
int js_array_push_number(js_value *arr, double v);                          // FUN_400e9430

const char *js_type_name(int type);                    // FUN_400edd44 ("boolean", ...)
