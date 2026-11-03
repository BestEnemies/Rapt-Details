# RAPT Details

Reverse-engineering notes, protocol specs, tools and replacement firmware for
KegLand **RAPT** brewing hardware — enough to decode each device's BLE packets
locally, and to control the Pill, without depending on the cloud.

Started as a RAPT Pill teardown; now organised per device.

## Devices

| Folder | Device | What's here |
|---|---|---|
| [`pill/`](pill/) | **RAPT Pill** hydrometer (ESP32-S0WD) | Hardware teardown, flash-dump + firmware analysis, the **BLE GATT command protocol** (live-verified), reconstructed application source, PlatformIO central that drives it, scripts |
| [`laserLevel/`](laserLevel/) | **RAPT Laser Level Sensor** | BLE advert decode — distance / battery read straight from the plaintext advert; live decoder |
| [`fermentationChamber/`](fermentationChamber/) | **RAPT FridgeG5** fermentation chamber | Its encrypted BLE beacon, plus **Chamber Screen** — a from-scratch ESP-IDF replacement firmware with every pin recovered from the stock image |
| [`common/`](common/) | shared | BLE advert framing used by all devices, and the **0xB1 encrypted-advert scheme** (AES-128, keyed per device) |

## Quick reference — reading the adverts

Every RAPT device advertises manufacturer data under company id `0x4152` whose
value starts `"PT"` + a **version byte** that tells you the format:

| version | device / format | decode |
|---|---|---|
| `0x01` / `0x02` | Pill, plaintext | [`pill/`](pill/) (also the public `rapt-ble` format) |
| `0xA1` | Laser level, **plaintext** | [`laserLevel/`](laserLevel/laser-ble-protocol.md) — distance is `byte[9]` mm |
| `0xB1` | **Encrypted** (FermChamber and others) | [`common/encrypted-advert-0xB1.md`](common/encrypted-advert-0xB1.md) — needs the device's `thingName` |
| `0xB7` | Pill 2.0.0, encrypted | not yet decoded (same family as 0xB1) |

See [`common/ble-advert-framing.md`](common/ble-advert-framing.md) for the byte layout.

## Tools

- `laserLevel/rapt_laser_decode.py` — scan and print laser distance live (and decrypt `0xB1` if you add a device's `thingName`).
- `pill/tools/pill_ble.py` — scan Pill telemetry; map GATT; send `{"cmd":…}` control writes.
- `pill/pill_central/` — ESP32 (PlatformIO) BLE central that commands the Pill.

## What is deliberately not in this repo

- **Firmware images / flash dumps** (`*.bin`). Dumps carry the device's own
  registration, Wi-Fi and cloud credentials in `nvs`; OEM images are KegLand's code.
  The analysis docs describe them; obtain your own.
- **The RAPT Brew APK** (118 MB, third-party). The decompiled findings are documented instead.
- **Per-device secrets.** The `0xB1` key is derived from a device's `thingName`, which
  is specific to your account's device — the docs show the format and how to fetch
  yours from brew.rapt.io, never a real one.
- Build output, `sdkconfig`, and `secrets.h` (see `.gitignore`).

## Status

- Pill: hardware, BLE command channel and OTA mechanism understood; owner-auth gate documented. ✅
- Laser level: advert fully decoded and verified against the portal reading. ✅
- FermChamber: `0xB1` beacon decryption proven; Chamber Screen firmware boots, drives all
  relays/inputs/display, runs the thermostat, alarms and humidity control.
  Open items are listed in each folder's README.

Contributions and corrections welcome — open an issue or PR.
