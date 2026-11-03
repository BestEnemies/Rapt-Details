# RAPT Pill 2.0.0 — reconstruction conventions & provenance

**What this is.** A reverse-engineered, *readable* C/C++ reconstruction of the
**application modules** of the RAPT Pill hydrometer firmware, version 2.0.0,
recovered from `app_2.0.0.bin` (ESP-IDF, ESP32-S0WD, Xtensa LE). It is **not**
the original source. It omits the ESP-IDF SDK (FreeRTOS, lwIP, mbedTLS, NimBLE,
NVS, esp_http_client, …), which is open source and unchanged.

**How it was produced.** Ghidra (PyGhidra) loaded the raw app image at its
ESP32 segment addresses and auto-analyzed it (6309 functions). Application
functions were isolated by their ESP-IDF log TAGs and the app-private rodata
string cluster (`0x3f4039xx–0x3f40cxxx`), then decompiled. The firmware was
built with assertions and `ESP_LOGx` left in, so **`__func__` names, assertion
expressions, and log format strings are embedded verbatim** in the binary — these
are the strongest evidence and drive the naming here.

## Conventions

- One file per recovered module under `reconstructed/main/`. The project is C++;
  files use a readable C-style C++.
- Provenance per function:
  `// <name> @ 0x400xxxxx  — <one-line purpose>`
  where `name` is the recovered `__func__`/symbol when known, else `fnXXXX`
  (from the Ghidra `FUN_0x400xxxxx` address).
- Real ESP-IDF / FreeRTOS API names are used wherever the callee was identified
  (`nvs_*`, `gpio_*`, `esp_sleep_*`, `esp_http_client_*`, `xTaskCreate`, …).
- **Honesty markers** — these are inferences from decompiled machine code:
  - `/* inferred */`      a name/type/constant deduced, not proven
  - `/* ? */`             genuinely uncertain
  - `// SDK: <api>`        a call resolved to an SDK function
  - `// FUN_0x...`         an app callee not yet reconstructed
- Struct layouts are given as byte offsets when the exact field is uncertain
  (`// +0x14`), or as named fields when a log/assert string reveals them.

## Confidence legend (per module header)

- **High** — logic, constants and strings all corroborate; behavior is clear.
- **Medium** — control flow is faithful; some types/names inferred.
- **Low** — skeleton only; semantics partly guessed, flagged inline.

## Device facts (from the binary and prior notes)

- Chip ESP32-S0WD rev1 single-core, 4 MB flash, not encrypted.
- Build: version string `2.0.0`; source `main.cpp`; uses lib tree
  `./libs/RAPT.Firmware.Common/…` (incl. vendored `esp32-wifi-manager`, `tinyexpr`).
- BLE name `RAPT Pill Hydrometer`; GATT base UUID `52415054-0a06-9215-08bf-f2b01884xx00`.
- Cloud: legacy `api.rapt.io/api/device/update` + AWS API Gateway
  `d78naeycz0.execute-api.ap-southeast-2.amazonaws.com` + AWS IoT shadow/jobs.
```
```
