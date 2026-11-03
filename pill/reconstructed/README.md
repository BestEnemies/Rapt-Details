# RAPT Pill — firmware 2.0.0, reconstructed application source

A reverse-engineered, **readable C/C++ reconstruction of the application layer**
of the RAPT Pill hydrometer firmware (version `2.0.0`), recovered from
`app_2.0.0.bin` with Ghidra. **This is not the original source** and does not
build as-is — it reconstructs *what the application does and how its modules fit
together*, module by module, with the Ghidra address of every function in a
comment. The ESP-IDF SDK (FreeRTOS, lwIP, mbedTLS, NimBLE, NVS, esp_http_client,
esp_ota) is **not** reconstructed; those are stock, open-source, and unchanged.

See `docs/RECONSTRUCTION_NOTES.md` for conventions, honesty markers, and how the
evidence was gathered. The original device analysis is in `../firmware-analysis.md`
and the live-verified BLE protocol in `../ble-protocol.md`.

## Device & toolchain (from the binary)

| | |
|---|---|
| MCU | ESP32-S0WD rev1, single core, 4 MB flash (unencrypted) |
| Framework | ESP-IDF 5.1 (NimBLE host), C++ |
| Entry | `app_main()` @ `0x400e1c78` (`main.cpp`) |
| Libraries | vendored `./libs/RAPT.Firmware.Common/…` incl. `esp32-wifi-manager`, `tinyexpr`, a cJSON-like JSON lib |
| BLE name | `RAPT Pill Hydrometer`; GATT base UUID `52415054-0a06-9215-08bf-f2b01884xx00` |
| Cloud | legacy `api.rapt.io`, AWS API Gateway `d78naeycz0.execute-api.ap-southeast-2`, AWS IoT device shadow + jobs |

## Boot & task model (`main.cpp`)

`app_main()` runs once and then hands control to the power-management run loop,
which owns the device's sleep/wake lifecycle. Order:

1. Read reset reason (`esp_reset_reason`); decide charger/battery state.
2. Quiet SDK log tags → WARN; `ESP_LOGI("Hello world!")`.
3. GPIO bring-up; `nvs_flash_init()`; `nonvolatile_init()`.
4. Subsystem init: `gravity_calc_init`, `calibration_run_init`,
   `automation_load_stored`, `measurement_init`, `power_management_init`,
   `wifi_manager_init`, `bluetooth_init`, `aws_iot_init`.
5. Global TLS CA store; OTA/firmware-info + rollback reporting; device name
   `RAPT-Pill-XXXXXX` from the STA MAC.
6. `xTaskCreate(networking_task, "networking", 0x3400)` and
   `xTaskCreate(iot_task, "iot", 8192)` (the latter from `aws_iot_init`); a
   dedicated **measurement** task with start/finished queues.
7. `power_management_run()` — never returns.

Tasks: `main` (→power loop), `networking`, `iot`, `measurement`, plus NimBLE host.

## Modules

| File | Role | Confidence |
|---|---|---|
| `main.cpp` | boot + init sequence + task creation | High |
| `bluetooth.cpp/.h` | GATT services, sub-char write/read routers, JSON **command dispatcher**, owner-auth | High |
| `gravity_calc.cpp/.h` | tilt→gravity via tinyexpr, calibration-run state machine, automation/alert engine | High/Med |
| `measurement.cpp/.h` | TMP1075 temp sensor (I²C), telemetry/report builder, settings cache | High/Med |
| `nonvolatile.cpp/.h` | templated typed NVS key/value layer (`NonVolatileKeyDef<T>`) | High |
| `power_management.cpp/.h` | power-state machine, battery curve/ADC, deep-sleep | High/Med |
| `ota.cpp/.h` | AWS + legacy firmware update check, checksum-gated install | High |
| `aws_shadow.cpp/.h` | AWS IoT device shadow desired/reported/delta, jobs, log upload | High/Med |
| `registration.cpp/.h` | device registration, auto-reg state, connectivity check, factory AP | High/Med |
| `wifi.cpp/.h` | glue over the vendored `esp32-wifi-manager` | Med |
| `web_config.cpp/.h` | soft-AP config portal (`/cal_1p.json`, `/factory_reset.json`, …) | Med |
| `accel.cpp/.h` | ST **LIS3DH** driver + double-tap/inversion **gesture** engine, status LED, diag export | High/Med |
| `json_util.h` | facade for the app's cJSON-like JSON accessor library (shared) | Med |

> **Coverage.** ~300 distinct application functions are reconstructed or cited by
> their Ghidra address; ~68% of the isolated app functions appear by address, the
> remainder being small wrappers folded into their module's grouped summary. The
> ESP-IDF SDK is deliberately excluded. Larger parsers/state machines are given at
> *structural fidelity* (faithful control flow, exact strings/constants) rather
> than line-by-line transliteration — see each file's inline notes.

### BLE command channel (the unique, live-verified part)

All control writes funnel through one router keyed by an internal
sub-characteristic id:

- `0x0C` owner-auth · `0x0E` command (`{"cmd","args"}`) · `0x0B` desired
  (`{"desired":…}`); reads: `0x01` device-info, `0x0B/0x0D` shadow, `0x0F` export.
- Commands (strcmp-dispatched in `command_dispatch`): `keep_awake`, `report_now`,
  `upload_log`, `start_automation`/`stop_automation`,
  `start_calibration_water`/`start_calibration_wort`, `cancel_calibration`,
  `clear_calibration`, `set_dev_options`, `set_beta_updates`, `clear_wifi`,
  `clear_registration`, `factory_reset` (interlocked on the device's own MAC),
  `export`.
- **Owner-auth** is a RAM-only flag + 64-byte secret, zeroed at every boot with
  no NVS copy; disabled until a session provisions a secret. See `bluetooth.h`.

### Gravity & calibration

Gravity is computed from the accelerometer tilt `angle` by evaluating the NVS
string `gravity_expr` with **tinyexpr**. Default (recovered verbatim):

```
(3.11683E-06*angle^5 - 0.000786275*angle^4 + 0.079045*angle^3
 - 3.96825*angle^2 + 100.097*angle - 1013.07)/1000 + 1
```

Calibration runs as a small state machine (phases settling→sampling→done/failed/
cancelled; states none/one_point/two_point/custom). See `gravity_calc.*`.

### Persistence (`nonvolatile.*`)

A templated `NonVolatileKeyDef<T>` registry over ESP-IDF NVS, value-cached and
mutex-guarded. Key table (short NVS key → type → default → meaning) is documented
at the top of `nonvolatile.cpp` and in `docs/NVS_KEYS.md`.

---

*Generated by reverse-engineering `app_2.0.0.bin`; see `docs/RECONSTRUCTION_NOTES.md`.*
