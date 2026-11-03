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

## Board pin map (FridgeG5 controller)

Recovered from the stock `fancyfridge` firmware by enumerating every
`gpio_set_level` / `gpio_get_level` / ADC / SPI call site and resolving its
constant argument (26 output sites, 25 input sites — the complete inventory, and
the recovery-mode diagnostics agree with the main application on every pin).
Everything marked **exact** was read from the instruction stream; all of it was
then **confirmed on real hardware (2026-08-31)**. Source of truth:
[`chamber-screen/firmware/main/board.h`](chamber-screen/firmware/main/board.h).

MCU: **ESP32-D0WD-V3** rev 3.0, 4 MB flash, WROVER module with PSRAM.

### Outputs — relay drivers, all active-high

| Function | GPIO | Notes |
|---|---|---|
| Heater | **18** | heating element |
| Cooler | **33** | compressor — see interlocks in the firmware README |
| Fan | **19** | circulation fan |
| Light | **21** | chamber light |
| Buzzer | **32** | audible alerts |

### Inputs — buttons and door, **active-low**

| Function | GPIO | Notes |
|---|---|---|
| Up | **15** | boot-strapping pin: held low at reset it silences the ROM boot log (harmless) |
| Down | **13** | |
| Select | **39** | input-only, no internal pull-up — board provides it |
| Back | **36** | input-only, no internal pull-up |
| Door | **35** | input-only, no internal pull-up |

The stock input-test screen shows seven columns (`up down select back pump door
play`) but reads only these five; `pump` and `play` are hard-coded `1`, i.e. not
fitted on this unit.

### Temperature probe — NTC on ADC

| | |
|---|---|
| Pin | **GPIO 34** = ADC1 channel 6 (the only ADC channel the stock firmware reads) |
| Config | 12-bit, 11 dB attenuation (`ADC_ATTEN_DB_12` in IDF v5) |
| Divider | 56 kΩ series resistor, thermistor on the **low** side: `R = 56000 · V / (3300 − V)` |
| Curve | `1/T = 1/T0 + (1/β)·ln(R/R0)` with stock defaults β = 3950, T0 = 298.15 K, R0 = 100 kΩ (NVS `ntc_beta0`/`ntc_t0`/`ntc_r0`) |
| Sampling | 128 samples, mean = sum ≫ 7; reading invalid if any raw sample is outside 2..4093 (open/short detection) |

Factory units also carry per-unit `cal_temp0/cal_adc0`, `cal_temp1/cal_adc1` and
`temp_offset` in NVS; an uncommissioned board reads several degrees low until
calibrated.

### Display — SPI panel (ST7789V class)

| Signal | GPIO | Notes |
|---|---|---|
| MOSI | **23** | SPI2 / HSPI, write-only (no MISO) |
| SCLK | **22** | 10 MHz (a 20 MHz variant exists) |
| CS | **5** | |
| DC | **2** | driven from the SPI pre-transfer callback via an *indirect* `callx8` — it does not show up in a plain scan of `gpio_set_level` call sites. Boot-strapping pin: fine as an output after boot |
| RST | **4** | high / 20 ms / low / 20 ms / high reset pulse in `probe_spi_init` |
| Backlight | **27** | active-high (this is **not** reset) |

Max transfer 10240 bytes. The panel is natively 240×320 portrait but is **mounted
rotated**: with the buttons along the bottom the usable UI is **320 wide × 240
tall**. It takes RGB565 **MSB-first** — an unswapped `0x001F` (blue) renders green.
The stock driver auto-probes the controller, so more than one panel variant ships;
don't assume ST7789V on a board you haven't checked.

### Spare expansion header — `3V3 / IO25 / IO26 / GND`

**Confirmed free**: no reference to either pin anywhere in the stock image, and no
DAC/LEDC/MCPWM/touch/ADC2/RMT/I2S/PCNT code is linked in.

- Neither is a strapping pin; both are RTC GPIOs (can wake from deep sleep).
- GPIO 25 = DAC1, GPIO 26 = DAC2 — the ESP32's only true analogue outputs.
- Caveat: they are also ADC2 channels 8/9, and **ADC2 is unusable while Wi-Fi runs** — treat them as digital/DAC, not ADC inputs.
- Chamber Screen uses them as **I²C** (`SDA = 25`, `SCL = 26`, 100 kHz) for an optional
  Sensirion SHT3x/SHT4x humidity sensor at `0x44`. Other good uses: 1-Wire
  (a factory-calibrated DS18B20 chain sidesteps NTC calibration), extra I²C sensors,
  or a second SSR output.

## Its BLE beacon

The chamber broadcasts an **encrypted `0xB1` advert**. The scheme — AES-128
keystream keyed by the device's `thingName` — is documented in
[`../common/encrypted-advert-0xB1.md`](../common/encrypted-advert-0xB1.md).

**What it contains: the live chamber temperature.** The 6 decrypted bytes are ASCII
hex text encoding 3 binary bytes:

```
"056000"  →  05 60 00
             ├─ 0x0560  int16 big-endian, 8.8 fixed point → ÷256 = +5.375 °C
             └─ 0x00    third byte: 0x00 in every packet so far (status/flags or padding)
```

Resolution is 1/16 °C (the raw value only moves in steps of 16). Verified on this
unit:

| when | decoded | situation |
|---|---|---|
| idle, held still | −0.81 °C, identical across 11 packets | fridge near its −2 °C target |
| normal cycling | −1.31 → −1.13 → −1.31 °C over a minute | portal built-in probe read −1.5 °C shortly before |
| **door opened** | **+5.4 → +5.9 °C and climbing ~1.5 °C/min** | portal had read −2.7 °C four minutes earlier |
| **door closed again** | **+3.1 → +2.3 °C, falling ~1.5 °C/min** (from the +5.9 peak 70 s earlier) | cold mass re-cooling the air at the probe |

The rise-and-fall on a door open/close is what a −2.7 °C chamber's air probe does, so
the beacon is the built-in probe, live — the cloud only samples it every ~45 minutes. The RAPT
Brew app itself never decodes this beacon (its live scanner handles only the laser
and AirStill); this layout is our finding. Setpoints and control state still come
from the cloud twin. Decoder: `../laserLevel/rapt_laser_decode.py` with the chamber's
`thingName` added to `THING_NAMES` — it prints the decrypted bytes; apply the ÷256 rule
to the first two decoded bytes.

## Updating & flashing — getting your own firmware on

What the stock firmware does for updates, and what that means for installing
Chamber Screen. All of this is read from the stock dump (`fridge_firmware.bin`,
partition table, strings and disassembly), on **this** FridgeG5 (ESP32-D0WD-V3).

### The device will boot anything you put in a slot

- **Flash encryption: OFF** — the dump is plaintext (thousands of readable strings;
  the whole pin map came out of it).
- **Secure boot: OFF** — no secure-boot-v2 signature block in the bootloader or
  either app slot. (The `secure_version` / appended-SHA-256 you see are the
  anti-rollback counter and an *integrity* hash, not an authenticity signature.)
- **Partition layout:** standard dual-OTA — `ota_0` @ `0x010000` and `ota_1` @
  `0x190000` (1.5 MB each), plus `otadata`, `nvs`, and a custom `history` partition.

So nothing cryptographic stops a custom image: whatever is written into an OTA slot
and selected in `otadata` will boot. The only question is **how to get it into the
slot without opening the case**.

### The stock OTA will not do it for you

The stock updater is **HTTPS-only and certificate-pinned**, so you cannot stand up a
look-alike server and have the device pull your image:

- The only update URL is `https://api.<domain>/api/device/update?macAddress=…&device=…&version=…`
  (plus the Azure/OAuth endpoints). **There is no `http://` OTA fallback** — the bare
  `http://%s` in the image is the Wi-Fi-setup captive portal, not firmware.
- TLS verifies the server against a **baked-in CA store**
  (`esp_tls_set_global_ca_store(root_ca, sizeof(root_ca))`). You can't present a
  trusted certificate for `api.<domain>`, so a DNS-redirect / MITM update is refused.
- Separately, that pinned store is the fixed four-root bundle whose **Baltimore
  CyberTrust Root expired 2025-05-12** (see the Chamber Screen README). That works
  *against* legitimate updates, not for injection.

You **can** stop the device updating or phoning home by sinkholing `api.<domain>` /
`id.<domain>` at your DNS — but that only pins it on its current firmware; it is a
suppression lever, not an install path.

### Classic ESP32 = UART ROM download only

The ESP32-D0WD-V3 has **no USB**; its ROM download mode is over **UART**. So a
genuinely no-solder first flash means non-soldered *contact* to the serial pins, not
a cable-free trick. You need TX, RX, GND, **IO0 held low at reset**, and an **EN**
strobe — the same five points used to take the dump (the Chamber Screen README
flashes over `COM7`). If those are a plated header or test pads, a **pogo-pin clip or
spring-loaded jig** does it with no iron; if they're bare vias you'll want a jig or a
temporary header. (Share a photo of the board and we can mark the exact points.)

Then write Chamber Screen into the slot and restore later if you want:

```bash
esptool --port COM7 --baud 460800 write_flash 0x10000 build/chamber_screen.bin
# or flash the whole stock image back to undo everything:
esptool --port COM7 --baud 460800 write_flash 0 fridge_firmware.bin
```

### After the first flash, it's cable-free forever

Chamber Screen brings its **own** OTA — an unauthenticated local endpoint on your
network, no cloud and no certificates:

```bash
curl --data-binary @build/chamber_screen.bin http://<device-ip>/api/ota
```

It writes the inactive slot and reboots, with bootloader rollback if the new image
can't get on the network (see the Chamber Screen README). So the hardware step is
**once**; every update after that is a `curl`. If you want zero hardware steps even
the first time, the honest answer on a no-secure-boot classic ESP32 is: there isn't
one that doesn't involve either the UART pins or the (cert-pinned) cloud.

## Not in the repo

`fridge_firmware.bin` (the stock flash dump — keep yours, it restores the device and
holds its registration), the OEM `ota_0.bin`/segment images and `disasm_*.txt`, and
`main/secrets.h` (Wi-Fi credentials). See the Chamber Screen README for why the dump
matters and how to restore it.
