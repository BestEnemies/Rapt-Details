# Laser Level Sensor — controls and settings

What the RAPT Brew app can read from and change on the laser level sensor, and how
it does it. Recovered from the decompiled app (`io.rapt.app` 1.1.5, blutter output;
mainly `generic_device_shell_screen.dart` → `_LaserSensorSetupState`,
`device_detail_shell.dart`, `vessel_registry.dart`,
`laser_keg_calibration_service.dart`, `laser_fill_alerts.dart`,
`laser_ble_telemetry_upload_service.dart`).

Confidence markers: **[exact]** read from a constant/string in the code;
**[inferred]** from names and surrounding logic — confirm against a live device.

## Two ways to reach the sensor

| Path | When | Notes |
|---|---|---|
| **Direct BLE (GATT)** | sensor awake and connectable | *"Connect to the sensor to change its settings."* In **Smart Mode** it is connectable at all times. Changes apply immediately. |
| **Cloud, queued** | sensor in **Ultra Low Power Mode** | *"Remote (queued) changes are only available while the sensor is in Ultra Low Power Mode."* The app writes the change to the platform at once — *"might take some time to reach the Laser Level Sensor"* — and the sensor picks it up on its next scheduled check-in. |

Cloud side, the app uses these GraphQL operations [exact]:
- `pushDeviceCommand(commands: [{ mac, deviceType: "LaserLevelSensor", operation, payload }])`
  — the generic command channel (also aliased `PushTwinUpdate`).
- `update_twin_properties` (an `operation`) — writes vessel/fill settings into the device twin's `desired` state.
- `ReportDeviceShadow` — reports the sensor's shadow back to the cloud after a direct BLE change.
- `EditDevice` / `UpdateDevice` — name, beer name, keg number (cloud-only metadata).
- `GetDeviceDetail`, `getDeviceData(get_device_twin)` — read state, including `thingName`.

### Command record shape [exact]

Every setting on the setup page is a record of `{ operation, shadowKey, value, changed }`.
`operation` is `set_<shadowKey>` and the same name is used whether the write goes
over BLE or via `pushDeviceCommand`. Example: shadow key `reading_offset_mm` →
operation `set_reading_offset_mm`.

### GATT [inferred from exact strings]

The laser uses the same RAPT GATT family as the Pill: service
`52415054-0a06-9215-08bf-f2b018840000`, with the laser page reading via
`_readMultiplexedJson` over characteristics `…840004` and `…840005` (a request/
response pair carrying JSON). The code is shared with the chamber
(`MiniChamberBleGattService`). The full characteristic set of that service family is
listed in `pill/ble-protocol.md`.

**Owner authentication** gates protected operations — e.g. *"Owner auth required for
laser live stream mode switch. thing_name is missing."* — and the auth secret is
derived from the device's **`thingName`** (*"Owner authentication requires non-empty
thing_name"*). The exact derivation isn't extracted yet; it is the same mechanism as
the Pill's owner-auth sub-characteristic (`0x0C`, see `pill/ble-protocol.md`).

## Operating modes (`mode`)

| `mode` | Name in app | Behaviour |
|---|---|---|
| `1` [exact] | **Smart Mode** | Recommended; connectable from the phone at all times; *"shorter battery life compared to Ultra-Low Energy mode"*. |
| `4` [exact] | **Ultra Low Power Mode** ("Ultra-Low Energy") | Sleeps between readings; battery *"up to 12 months"*; settings changes are queued via the cloud. |
| ? | **Session Mode** | A live-stream session (`enable_session_mode`); *"Session Mode Enabled"*, ends with *"Stop Live Stream"*. |
| ? | **Custom Advanced Mode** | *"Fully configure the chip to your specific setup."* |

Related keys: `mode`, `mode_name`, `keepCurrentMode`, `wakeup_via_bluetooth`,
`set_reading_mode_raw` [exact names]. Session/Custom mode numbers are not yet pinned.
The sensor reports its current mode; an unrecognised value shows *"Unknown Mode (n)"*.

## Settings — shadow keys and their set-commands

### "Laser Reading Settings"

| Shadow key [exact] | Operation [exact] | App label | Values |
|---|---|---|---|
| `telemetry_frequency` | `set_telemetry_frequency` | Telemetry Frequency | 5 min / 15 min / 30 min / 1 hour (stored in seconds; 60, 3600, 7200 appear as constants) |
| `reading_offset_mm` (wire variant `reading_offset_mm_x10`) | `set_reading_offset_mm` | Reading Offset (mm) | mm correction added to the raw distance |
| `time_between_measurements_seconds` | — | Reading Interval (sec) | seconds between measurements |
| `idle_time_seconds`, `idle_time_seconds_registered`, `idle_time_seconds_session` | — | — | sleep intervals: unregistered / registered / in a session |
| `keep_alive_beacon_amount` | `set_keep_alive_beacon_amount` | — | beacons sent to stay discoverable |
| `low_battery_alert_percent` | — | — | low-battery threshold |
| `signal_strength_level` | — | — | radio power level [inferred] |
| `stop_session_amount_no_change` | — | — | end a session after N unchanged readings [inferred] |

### "Live Stream Settings" (session mode)

| Shadow key [exact] | Operation [exact] | App label |
|---|---|---|
| `live_stream_data_above_mm_per_min` (`_x10` on wire) | `set_live_stream_data_above_mm_per_min` | Live Stream Data Above — stream only while the level is changing faster than this |
| `stop_stream_after_seconds` | `set_stop_stream_after_seconds` | Stop Stream After |
| `stop_stream_when_below_mm_per_min` (`_x10` on wire) | `set_stop_stream_when_below_mm_per_min` | Stop Stream When Below |
| `enable_session_mode` | — | Preview Live Stream / Stop Live Stream |

### Fill levels (device twin `desired`) [exact]

`fill_level_alert_mm`, `fill_level_notification_mm`, `fill_level_empty_mm` — distances
(mm) at which the sensor flags alert / notification / empty.

### Vessel definition (twin `desired`, pushed via `update_twin_properties` or written directly over BLE) [exact]

| Key | Meaning |
|---|---|
| `keg_type_id` | `keg_19l` ("19L Keg"), `keg_9_5l` ("9.5L Keg"), `keg_7_5l` ("7.5L Keg") |
| `vessel_empty_distance_mm` | distance reading when empty |
| `vessel_full_distance_mm` | distance reading when full |
| `vessel_full_volume_ml` | volume at full |

**Custom vessels** are defined app-side (registry `assets/config/vessel_sizes.json`
plus user `custom_vessels`): `capacity_liters` (default **19.0**), `total_height_mm`
(default **563**), `offset_mm`, `min_reading_mm`/`max_reading_mm`, and an optional
`custom_measurement_map` of `distance_mm → remaining_l` for non-cylindrical vessels.
Without a map the app does a simple linear mm→L conversion (*"Used for the simple
linear conversion from raw mm to L. Override below for non-cylindrical vessels."*).
Per-vessel display calibrations (`assets/calibrations/laser_sensor_<id>.json`) hold
`fill_height_at_5_percent`, `fill_height_at_100_percent` and
`blinking_threshold_percent` for the keg graphic.

### Metadata (cloud only, `EditDevice`)
Device name (required at setup), **Beer Name** (`beer_name`), **Keg Number**
(`keg_number`) — the app suggests writing the keg number on the sensor lid.

## Actions

- **Request current reading / wake** — `get_last_telemetry`; the app connects, asks
  for a reading, and times out if none arrives (*"Timed out waiting for reading"*).
- **LED** — a BLE write when connected, otherwise a remote command (`[LASER_LED]`).
- **Firmware update over BLE** (`[LASER_OTA]`) — downloads the image, checks battery
  first, writes it, then reconnects and verifies.
- **Fill alerts** — per-device watchdogs (`DEF_<mac>_FILL-…`) that fire
  *"{device} is below … L"* / *"Only {n} L left in the keg (alert level: …)"*, with a
  5-minute cooldown; re-armed when the level recovers.
- **Registration** — `POST …/v1/device/registerBLEDevice` (`ble_register`,
  `registerIntoExisting`).
- **Telemetry relay** — when the phone hears a reading over BLE it re-uploads it:
  `POST https://q01sah2ad9.execute-api.ap-southeast-2.amazonaws.com/v1/upload` with
  `{macAddress, deviceType:"LaserLevelSensor", rawReading, remainingVolume, kegSize,
  battery, batteryState (critical|low|medium|high|unknown), firmwareVersion, rssi,
  source:"mobile-ble", version, createdOn}`, throttled to one upload per 30 s per
  device [exact field names].

## Reading the result

The advert itself needs none of this — see `laser-ble-protocol.md`. The cloud and
app derive `remainingVolume` from the raw distance using the vessel definition above;
the portal exposes both (`rawReading`, `remainingVolume`).

## Open items
- Pin the `mode` numbers for Session and Custom Advanced modes.
- Extract the owner-auth derivation from `thingName` (needed to issue protected
  commands from your own client).
- Capture a real `pushDeviceCommand` payload and a direct GATT write to confirm the
  JSON shape on the wire.
