# RAPT Pill firmware — dump and image comparison

Analysis of a RAPT Pill (KegLand hydrometer) whose flash was read over a
serial adapter on 2026-10-05, plus the two firmware images the RAPT update
server returns for it.

## Device

| | |
|---|---|
| Chip | ESP32-S0WD rev v1.0 (single core) |
| Flash | 4 MB, not encrypted |
| MAC | `c4:5b:be:b2:be:c0` |
| Advertised name | `RAPT Pill bec0` |
| Project | `rapt-hydrometer` |

## Flash dump

Full 4 MB read twice; both reads hashed identically, so the dump is clean.

- `pill_firmware.bin` / `pill_firmware_verify.bin`
- SHA-256 `62c16d12b4edb522f9f1fb08a4d1e32388d82d05feaffde4754be0322dcaebc2`

> The `nvs` partition in this dump holds the device's own registration and
> Wi-Fi state. Keep the `.bin` private; don't share it as-is.

### Partition table (`@0x8000`)

| Name | Type | Offset | Size | Contents |
|---|---|---|---|---|
| otadata | data | 0x00d000 | 0x002000 | boot-slot selector |
| phy_init | data | 0x00f000 | 0x001000 | RF calibration |
| ota_0 | app | 0x010000 | 0x180000 | build `20250319_055542_b3788ba` (Mar 2025) |
| ota_1 | app | 0x190000 | 0x180000 | build `20230420_054501_98ef1a1` (Apr 2023) — **active** |
| nvs | data | 0x310000 | 0x080000 | device settings / registration |

otadata points at `ota_1`, so the Pill is running the **April 2023** build.
The March 2025 image in `ota_0` is a leftover the server has since rolled
back from (see below).

## Update mechanism

The running firmware checks for updates with a single unauthenticated GET,
built in `main/ota.c` (`ota_do_update`):

```
https://api.rapt.io/api/device/update?macAddress=<mac>&device=<device>&version=<version>
```

- Host is `rapt.io`, or `test.rapt.io` when the `test_api` NVS setting is on.
- `<device>` is `PillG1` (or `PillG1-beta`).
- `<version>` is the running build string.
- The response body **is** the firmware image, streamed straight into the
  spare OTA slot. An empty 200 means "up to date".
- It refuses to start unless the charger is connected and Wi-Fi is up.

### Server responses observed (2026-10-05)

Requests sent with this Pill's MAC:

| device | version sent | response |
|---|---|---|
| PillG1 | `20230420…` (Apr 2023) | empty 200 → up to date |
| PillG1 | `20250319…` (Mar 2025) | the **Apr 2023** image, 1,103,456 B |
| PillG1-beta | either | version **1.5.0**, 1,190,784 B |

So the current production build for a `PillG1` is the April 2023 image — the
server downgrades the 2025 build back to it. The `PillG1-beta` channel
returns a much newer build.

## Image comparison — Release vs Beta

Both are standard ESP-IDF app images for the same `rapt-hydrometer` project.

| | Release | Beta |
|---|---|---|
| File | `server_PillG1_20230420_054501_98ef1a1.bin` | `server_PillG1-beta_1.5.0.bin` |
| SHA-256 | `df0efef243b414f1eebb99aa7520dd554e69507dce0df2632cbf846e0ee6c949` | `6a755d6bd3ecdd9bce04050b9d7580e8676ba770ae5522ddad4b2434a54cb651` |
| Version | `20230420_054501_98ef1a1` | `1.5.0` |
| Built | 2023-04-20 05:45:11 | 2026-10-01 17:13:07 |
| ESP-IDF | v4.3.2 | v5.1.7-1-g4b1ae715b8 |
| Image size | 1,103,416 B | 1,190,744 B |
| Code | 860,404 B | 922,748 B |
| Rodata | 207,156 B | 232,832 B |

### Unchanged between the two

Same application modules — `authentication`, `bluetooth`, `gravity_calc`,
`main`, `measurement`, `nonvolatile`, `ota`, `power_management` — and the
same local config web pages and `/*.json` endpoints served by the Pill's
soft-AP (`settings.html`, `calibrate.html`, `diag.html`, `register.html`,
`sensors.json`, `set_test_api.json`, `set_beta.json`, `ota_now.json`, etc.).

The legacy `api.rapt.io/api/device/update` path is still present in the beta.

### New in the beta

- **A second update path, via AWS.** Alongside the legacy check, the beta
  queries an AWS API Gateway endpoint in `ap-southeast-2`:
  - `…execute-api.ap-southeast-2.amazonaws.com/v1/devices/firmwareVersionCheck-wifi?thingType=…&firmwareVersion=…&hardwareVersion=…&macAddress=…`
  - `…execute-api.ap-southeast-2.amazonaws.com/v1/devices/azureCheck`

  The response is JSON carrying `updateAvailable`, `otaTarget`, a
  `downloadUrl`, and a SHA-256 checksum the firmware **refuses to install
  without**. It falls back to the legacy server if the AWS check fails, and
  records in NVS (`ota_aws`) when an AWS image is installed or rolled back by
  the bootloader.
- **RAPT Brew migration.** The settings page gains a popup prompting the user
  to install the RAPT Brew app and migrate the Pill. A commented-out note in
  the page source says the old RAPT Portal is deprecated on 2026-12-31.
- **New `accel.c` module** and a move to the newer ESP-IDF 5.1 BLE stack
  (NimBLE host error-string tables are now compiled in).

## Files in this folder

| File | What |
|---|---|
| `pill_firmware.bin` | 4 MB flash dump (sensitive — contains nvs) |
| `pill_firmware_verify.bin` | second read, identical hash |
| `server_PillG1_20230420_054501_98ef1a1.bin` | release image from update server |
| `server_PillG1-beta_1.5.0.bin` | beta image from update server |
| `pill_esptool.py` | esptool wrapper that opens the port with RTS/DTR released (needed while RTS is wired to EN) |

## Serial / recovery notes

- FTDI on COM7 at 3.3 V; TXD↔RXD crossed; RTS→EN, IO0 held to GND for boot mode.
- Plain `esptool` fails because opening the port holds RTS (→EN) low, pinning
  the chip in reset. Use `pill_esptool.py` while RTS is on EN.
- To restore the Pill exactly: `esptool write_flash 0 pill_firmware.bin`
  (keeps nvs intact). Nothing has been written to the device during this work.
