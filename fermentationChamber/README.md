# RAPT FridgeG5 fermentation chamber

KegLand's fermentation-chamber controller (the "chamber" device in the RAPT app;
portal device type `FermChamber`). ESP32-D0WD-V3, 4 MB flash, WROVER module with
PSRAM, driving a compressor, heater, fan, light, buzzer and a small SPI panel.

## Contents

- [`chamber-screen/`](chamber-screen/) — **Chamber Screen**, a from-scratch ESP-IDF
  replacement firmware for the controller. Start with its
  [README](chamber-screen/firmware/README.md): build/flash, the web UI and OTA
  loop, thermostat, compressor interlocks, alarms, humidity/reheat control, and
  passive RAPT Pill scanning. Every pin in `main/board.h` was recovered from the
  stock image and confirmed on hardware.
  - `firmware/` — the ESP-IDF project (source in `main/`).
  - `design/` — the panel screen designs the LVGL UI implements.
  - `fridgeg5-map.html`, `segments.json`, `xrefs.json` — the stock-firmware
    analysis the pin map came from.

## Its BLE beacon

The chamber broadcasts an **encrypted `0xB1` advert**. The scheme — AES-128
keystream keyed by the device's `thingName` — is documented in
[`../common/encrypted-advert-0xB1.md`](../common/encrypted-advert-0xB1.md). On the
unit tested, the decrypted 6 bytes are an ASCII colour hex (`ff3000`, cycling
through `feb000…fee000`): a status/LED colour rather than temperature. The
temperature and setpoints come from the cloud twin, not the beacon.

## Not in the repo

`fridge_firmware.bin` (the stock flash dump — keep yours, it restores the device and
holds its registration), the OEM `ota_0.bin`/segment images and `disasm_*.txt`, and
`main/secrets.h` (Wi-Fi credentials). See the Chamber Screen README for why the dump
matters and how to restore it.
