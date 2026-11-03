# RAPT Pill 2.0.0 — NVS key reference (reverse-engineered)

Keys stored by the application through the `nonvolatile` layer
(`NonVolatileKeyDef<T>`, mutex-guarded, value-cached over ESP-IDF NVS).
Short NVS key names (≤15 chars) are what is written to flash; the JSON/device-
shadow documents use the longer aliases shown. Types and defaults are inferred
from the typed getters and their fallback values. Confidence: High for the key
set, Medium for a few defaults (flagged).

| NVS key | Type | Default | Shadow/JSON alias | Meaning |
|---|---|---|---|---|
| `registeredaws` | u32 | 0 | `registered` | device registered with the AWS backend |
| `test_api` | bool | 0 | `test_api` | use `test.rapt.io` instead of `api.rapt.io` |
| `cal_state` | u32 | 0 | `calibration_state` | 0 none / 1 one_point / 2 two_point / 3 custom |
| `dev_opt` | bool | 0 | `dev_options` | developer options enabled |
| `beta_updates` | bool | 0 | `beta_updates` | beta OTA channel |
| `log_int` | u32 | 172800 s (48 h) | `log_upload_interval` | periodic log-upload interval |
| `log_whenfull` | bool | 1 | `log_upload_when_full` | also upload when the log buffer fills |
| `telem_method` | u32 | 1 | `telemetry_method` | 0 none / 1 wifi / 2 ble /* inferred mapping */ |
| `interval_w` | u32 | — | `telemetry_interval_wifi` | telemetry period over wifi |
| `interval_b` | u32 | 300 s | `telemetry_interval_ble` | telemetry period over BLE |
| `grav_v_time` | u32 | 86400 s | `gravity_velocity_time` | window for the gravity-velocity slope |
| `grav_v_age` | u32 | — | `gravity_velocity_age` / `_oldest_age` | ages used in the slope calc |
| `low_batt` | bool | 0 | `low_battery_shutdown` | shut down early on low battery |
| `smart_mode` | bool | 1 | `smart_mode` | smart connectable-window scheduling |
| `cal_d1`,`cal_a1` | float | — | `calibration_density[0]`,`calibration_angle[0]` | calibration point 1 |
| `cal_d2`,`cal_a2` | float | — | `calibration_density[1]`,`calibration_angle[1]` | calibration point 2 |
| `cal_points` | blob | — | `calibration_points` | stored calibration point set |
| `cal_run` | blob | — | `calibration_run` | in-progress calibration run state |
| `gravity_expr` | str | *default polynomial* | `gravity_expr` | tinyexpr gravity-from-angle formula |
| `gravity_expr_valid` | bool | — | `gravity_expr_valid` | last stored expression compiled OK |
| `adc_cal` | float | — | `adc_calibration` | battery ADC offset (self-calibrated on charger) |
| `temp_offset` | s32 | 0 | `temperature_offset` | temperature calibration offset |
| `refresh_tok` / `refresh_token` | str | — | `refresh_token` | cloud refresh token |
| `aws_cert` | str | — | — | AWS IoT client certificate (PEM) |
| `aws_key` | str | — | — | AWS IoT client private key (PEM) |
| `thingName` | str | — | `thingName` | AWS IoT thing name from registration |
| `fw_reported` | str | — | `fw_reported` | last firmware version reported to the cloud |
| `fw_failure` / `fw_rollback` | — | — | — | OTA failure / bootloader-rollback flags |
| `automation` | blob (0x710) | — | — | stored fermentation automation + alert rules |
| `ble_ver` | blob | — | — | BLE service version |
| (wifi, namespace `espwifimgr`) | blob | — | `sta.ssid` | SSID/password saved by esp32-wifi-manager |

The **default gravity expression** (key `gravity_expr`) recovered verbatim:

```
(3.11683E-06*angle^5 - 0.000786275*angle^4 + 0.079045*angle^3
 - 3.96825*angle^2 + 100.097*angle - 1013.07)/1000 + 1
```

Note: `aws_cert`, `aws_key`, `refresh_tok`, `thingName`, and the wifi credentials
are **secrets** present in a device's NVS. The 4 MB flash dump
(`pill_firmware.bin`) contains these for the dumped unit — keep it private.
