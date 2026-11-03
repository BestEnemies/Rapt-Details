# Chamber Screen — replacement firmware for the RAPT FridgeG5

Bring-up firmware for the KegLand RAPT FridgeG5 fermentation chamber
controller (ESP32-D0WD-V3 rev v3.0, 4 MB flash, WROVER module with PSRAM).

Every pin in `main/board.h` was recovered from the stock `fancyfridge`
firmware by disassembly. The full analysis is in
[`../fridgeg5-map.html`](../fridgeg5-map.html).

## Before you flash anything

**Keep `../fridge_firmware.bin`.** It is a complete 4 MB dump of the stock image,
including the `nvs` partition that holds your device registration and OAuth
refresh token. Restoring it puts the controller back exactly as it was:

```bash
esptool --port COM7 --baud 460800 write_flash 0 fridge_firmware.bin
```

`partitions.csv` reproduces the stock layout byte for byte so that restore
stays valid and `nvs` is never disturbed.

## Build

Needs ESP-IDF v5.x. Verified against v5.4.

```bash
idf.py set-target esp32
```

```bash
idf.py build flash monitor -p COM7
```

## WiFi credentials

Edit the two lines in [`main/secrets.h`](main/secrets.h) and rebuild:

```c
#define CS_WIFI_SSID_OVERRIDE       "your-ssid"
#define CS_WIFI_PASSWORD_OVERRIDE   "your-password"
```

That file is in `.gitignore`. It overrides the menuconfig values, so a
password never lands in `sdkconfig` or in version control. If the file is
absent the build falls back to `idf.py menuconfig` → Chamber Screen.

WiFi failure is deliberately non-fatal. Temperature control must never depend
on a network, so a failed connection is logged and the controller carries on
without the web UI.

## Development loop

After the first USB flash you should not need the cable again.

Browse to `http://<device-ip>/` — the IP is logged at boot and shown by
`idf.py monitor`. The page gives you:

- **Live log tail**, polled from an in-memory ring buffer. Serial output is
  unaffected, so `idf.py monitor` keeps working simultaneously.
- **Live status** — all five inputs, all five outputs, probe reading,
  compressor runtime and start counts, and how long protection is blocking.
- **Relay buttons**, so you can verify wiring from a laptop instead of
  crouching at the chamber.
- **Firmware upload**, which writes the inactive OTA slot and reboots.

Command-line push, which is usually faster than the browser:

```bash
curl --data-binary @build/chamber_screen.bin http://192.168.1.50/api/ota
```

Use the device's IP. The build sets a DHCP hostname (`fridge` by default) but
does **not** run mDNS, so `fridge.local` resolves only if your router publishes
DHCP hostnames in DNS. Add the `espressif/mdns` managed component if you want
`.local` to work reliably.

### Rollback — why a bad push is cheap

`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` is on. An OTA image boots in
`PENDING_VERIFY` and only becomes permanent when it calls
`esp_ota_mark_app_valid_cancel_rollback()`. This firmware calls it **only
after the web server is serving**.

So a build that crashes, hangs, or cannot reach WiFi gets reverted to the
previous slot on its next reboot. That is deliberate: an OTA image that cannot
get on the network is one you could not push a fix to.

Images flashed over USB are unaffected — they are not in `PENDING_VERIFY`, so
a cabled flash always sticks.

### Security

**The web UI has no authentication.** Anyone on the network can toggle your
compressor and upload firmware. It is a bench tool. Turn it off in menuconfig
(`CS_WEBUI_ENABLE`) before this goes anywhere you do not control.

## What this build does

**Buttons are observation-only.** Each press, release and long-press is logged
with its duration and nothing else happens — so handling the panel during
testing can never energise an output by accident. Relay control lives in the
web UI.

A status line prints once a second with every input, every output and the
probe reading.

The KegLand cloud client is deliberately absent.

### Thermostat

Hysteresis control with a proper cut-in / cut-out deadband:

```
cooling starts when  temp > target + cooler_hys
cooling stops  when  temp <= target
heating starts when  temp < target - heater_hys
heating stops  when  temp >= target
```

Stopping at the target rather than at the opposite trip point means each cycle
actually reaches the setpoint instead of oscillating between the two
thresholds, and it maximises compressor off-time.

Deliberately **not PID**, despite the stock firmware exposing `pid_p`/`pid_i`/
`pid_d`. A fridge compressor is a fixed-output device — PID only helps if you
can modulate, and modulating a compressor is how you destroy it.

The thermostat decides *intent*; the relay layer still decides whether the
compressor may actually start. A refused request is normal, not an error.

It starts **disabled**, and its configuration persists in NVS.

```bash
curl -X POST "http://<ip>/api/thermostat?enabled=1&target=18.5&source=pill&fan=always_on&save=1"
```

| parameter | values |
|---|---|
| `enabled` | `0` / `1` |
| `target` | °C, must sit within `min_temp`..`max_temp` |
| `cooler_hys`, `heater_hys` | °C, must be positive |
| `source` | `internal` (wired NTC) or `pill` (BLE) |
| `pill` | sensor address; empty = most recently heard |
| `fan` | `off`, `with_active`, `always_on` |
| `save` | `1` to persist to NVS |

**Fan modes.** `always_on` is honoured even when the thermostat is disabled —
that is the point of it. Note this means manual fan control from the relay
buttons is overridden within one control tick (5 s).

**Sensor staleness.** If the selected Pill has not been heard for
`sensor_timeout_s` (default 300), the thermostat faults to safe rather than
continuing to act on a frozen reading. A chamber cooling against a stuck
sensor value is how you freeze a ferment.

### Panel UI (LVGL)

LVGL 9.5 via `espressif/esp_lvgl_port`, implementing the designs at
<https://claude.ai/code/artifact/29af143e-d992-42b6-aadb-2c37e9cec8f5>.
Currently the main status screen; the rest are designed but not yet built.

Four things about this integration are non-obvious and cost a boot loop each:

**Init order.** `ui_init()` must come *after* `thermostat_start()` and
`pill_start()`. It reads from both, and calling it early dereferenced their
mutexes before they existed — a null-pointer panic on the first frame, before
`mark_app_valid`, so the bootloader rolled the image back. The getters in
`thermostat.c`, `pill.c` and `humidity.c` now return safe defaults when called
before their init, so this class of mistake cannot panic again.

**No I/O on the render task.** An early version called `temp_probe_read()` from
the 1 Hz LVGL timer — 128 ADC conversions inside a render callback on a 4 KB
stack. The UI draws cached state; acquisition belongs to the control loop.

**The draw buffer lives in PSRAM.** At 240×40×2 it is ~19 KB, and keeping it
in internal DRAM overflowed `dram0_0_seg` — WiFi, BLE and TLS have already
claimed most of it. PSRAM cannot be a DMA source for SPI on the ESP32, so the
port copies instead; at one refresh a second that costs nothing measurable.
The static log ring buffer was also cut from 12 KB to 6 KB for the same reason.

**LVGL must be trimmed.** By default it builds its examples, demos, test
harness, monkey tester, sysmon and ThorVG shims. The partition layout is fixed
to match stock, so the app has to fit — see the LVGL block in
`sdkconfig.defaults`. `.swap_bytes = true` in the port config handles the
panel's big-endian RGB565 properly, replacing the per-pixel swap the bring-up
fill used.

Outstanding: the design's 80 px numeral renders at 48, LVGL's largest built-in
size. Matching the design needs a font generated with `lv_font_conv` —
digits, a dot, a minus and a degree sign are enough.

### Alarms

Seven conditions, using the stock firmware's own wording so the device reads
the same to anyone who has used an original unit.

| id | condition |
|---|---|
| `probe` | probe open or shorted |
| `sensor_lost` | controlling sensor went quiet **while controlling** |
| `sensor_diff` | the two sensors disagree by more than `max_sensor_diff_c` |
| `over` / `under` | outside `alarm_high_c` / `alarm_low_c` |
| `unreachable` | off target by `unreachable_dev_c` for `unreachable_s` **while working** |
| `door` | door held open for `door_open_s` |

**An alarm is a latch, not a level.** Once raised it stays raised until the
condition clears *and* someone acknowledges it. The whole point is to survive
being unobserved — a door left open for an hour overnight must still be
visible in the morning even though it is shut by then.

**Silence is not dismiss.** Silence stops the buzzer and leaves the alarm
showing; dismiss clears it. Conflating them means the only way to stop the
noise is to destroy the evidence. Dismissing an alarm whose condition is still
true is **refused** — it silences instead — because an alarm that will not go
away is telling you something, and pretending the button worked is worse than
saying it did not.

```bash
curl -X POST "http://<ip>/api/alarm?action=silence"
curl -X POST "http://<ip>/api/alarm?action=dismiss&id=door"
```

Two design points worth keeping if you rework this:

**Severity order puts sensing faults above everything derived from a
measurement.** A bad reading can manufacture a plausible-looking
over-temperature alarm; the sensor fault is the one worth showing.

**Conditions are gated on whether they can mean anything.** `unreachable`
only counts while the outputs are actually working — sitting far from target
with everything idle is the deadband doing its job, not a fault. `sensor_lost`
only counts while the thermostat is enabled; with control off there is no
controlling reading *by definition*, and raising an alarm for that is the
fastest way to teach someone to ignore the alarm panel. Range alarms, by
contrast, stay armed from the probe even when control is off — a chamber at
45 °C is worth knowing about regardless.

The buzzer beeps 200 ms on / 1800 ms off for 30 s, then stops on its own.

### Humidity and reheat dehumidification

A Sensirion **SHT3x or SHT4x** on the spare header (`SDA = GPIO 25`,
`SCL = GPIO 26`) enables humidity control. Both parts live at 0x44, return the
same CRC-protected frame, and are told apart at init. Absence is normal and
non-fatal — the sensor is simply reported as `none` and dehumidification
refuses to enable.

Every reading is CRC-checked. In a wet chamber a corrupted word that happens
to look plausible would drive the compressor, so bad frames are discarded
rather than used.

**How it works.** Reheat dehumidification runs the compressor to drag the
evaporator below dew point so moisture condenses out, while the heater cancels
the resulting sensible cooling. Moisture leaves; temperature doesn't move. It
is how commercial curing chambers work.

**It only makes sense if your evaporator drains.** If the coil merely frosts,
the water returns at the next defrost and all you get is a frosted coil and a
large electricity bill. Check for a drain hole at the back of the chamber
floor before enabling this.

**Safety envelope.** Running two opposing actuators against each other turns a
stuck sensor from an annoyance into a hazard, so reheat is gated on everything
being demonstrably healthy and drops out the moment anything is not:

| gate | behaviour |
|---|---|
| humidity sensor missing or stale | never enters reheat |
| temperature invalid or stale | `emergency_stop`, which also revokes permission |
| temperature strays > `reheat_max_dev` (default 2 °C) | aborts |
| continuous run > `reheat_max_min` (default 30) | forced rest of `reheat_rest_min` (default 15) |

The compressor's minimum off time and minimum run time still apply — reheat
relaxes only the mutual exclusion and the heat/cool dwell, nothing else.
`relays_allow_simultaneous()` is off by default and `relays_emergency_stop()`
revokes it, on the principle that whatever was supervising it has by
definition just lost control.

The fan is forced on during reheat: circulation drives both the condensation
and the heat distribution, so it isn't optional.

```bash
curl -X POST "http://<ip>/api/thermostat?humidity=1&rh_target=80&rh_hys=3&save=1"
```

| parameter | default | notes |
|---|---|---|
| `humidity` | `0` | refuses to enable with no sensor present |
| `rh_target` | 80 | percent, must be 20..95 |
| `rh_hys` | 3 | starts above target+hys, runs until back at target |
| `reheat_max_dev` | 2.0 | °C, abort threshold, capped at 5 |
| `reheat_max_min` | 30 | duty cap |
| `reheat_rest_min` | 15 | forced rest after the cap |

### RAPT Pill / RAPT Temp

Passive NimBLE observer, so the sensor's battery is unaffected by us
listening. Handles v1 and v2 frames, up to four sensors, with age tracking.
Readings appear in `/api/status` under `pills` and in the browser UI.

Frame format per KegLand's published spec and the reference parser at
<https://github.com/sairon/rapt-ble>, verified against that project's test
vectors. Everything after the 2-byte company ID is **big endian**, which is
unusual for BLE and easy to get wrong:

| offset | size | field |
|---|---|---|
| 0 | 2 | company id `"RA"` (0x4152 LE) or `"KE"` (0x454b LE) |
| 2 | 2 | magic `"PT"` |
| 4 | 1 | version (1 or 2) |
| 5 | 6 | v1: sensor MAC — v2: `[0]` reserved, `[1]` velocity valid, `[2..5]` float32 |
| 11 | 2 | temperature uint16 → `/128 - 273.15` = °C |
| 13 | 4 | gravity float32 → `/1000` = SG |
| 17 | 6 | accel X/Y/Z int16 → `/16` |
| 23 | 2 | battery int16 → `/256` = % |

NimBLE is configured observer-only with `BT_NIMBLE_HOST_BASED_PRIVACY=n`.
Leaving privacy enabled pulls in `ble_hs_resolv`, which then wants
`ble_sm_alg_encrypt` from the security manager that observer-only mode
removes — a link error rather than a config error.

The `NimBLE: Failed to persist local IRK (rc=8)` warning at boot is benign.

## What is verified

Everything in `board.h` was recovered from the stock firmware and has since
been **confirmed on real hardware**:

| | |
|---|---|
| Outputs | heater 18, cooler 33, fan 19, light 21, buzzer 32 — all toggled |
| Inputs | up 15, down 13, select 39, back 36, door 35 — all pressed, active-low |
| Probe | ADC1 ch6 (GPIO 34), 12-bit, 128-sample averaging, fault window |
| Display | MOSI 23, SCLK 22, CS 5, DC 2, RST 4, backlight 27 — drawing |
| Interlocks | compressor refused an early restart under real conditions |

Two pins were harder than the rest and are worth knowing about:

- **DC is GPIO 2**, set by the SPI pre-transfer callback at `0x40090660` via an
  indirect `callx8` to `gpio_set_level`. It does not show up in an enumeration
  of `gpio_set_level` call sites, which is why the first two attempts failed.
- **RST is GPIO 4**, identified by its high/20 ms/low/20 ms/high pulse in
  `probe_spi_init`; **GPIO 27 is the backlight**, not reset.

The panel takes RGB565 **most-significant byte first** — `display_fill()`
swaps accordingly. An unswapped `0x001F` (blue) renders as green.

Still unconfirmed: **panel geometry** (`BOARD_LCD_H_RES`/`V_RES` are assumed
240x320).

### The spare header (3V3 / IO25 / IO26 / GND)

**Confirmed free.** An exhaustive scan of every `gpio_set_level`,
`gpio_get_level` and `gpio_set_direction` call site — including indirect calls
through a function pointer, which is how DC on GPIO 2 escaped an earlier pass
— finds no reference to either pin. No DAC, LEDC, MCPWM, touch, ADC2, RMT,
I2S or PCNT code is linked into the stock image either.

Both pins are well behaved: neither is a strapping pin, both are RTC GPIOs
(so both can wake from deep sleep), and GPIO 25/26 are DAC1/DAC2 — the
ESP32's only true analogue outputs.

**Caveat:** they are also ADC2 channels 8 and 9, and **ADC2 cannot be used
while WiFi is running**. Treat them as digital or DAC pins, not ADC inputs.

Good candidates here: **1-Wire** (a DS18B20 chain is factory-calibrated,
sidestepping the NTC calibration problem entirely), **I2C** for extra sensors,
or a second SSR output.

## The temperature curve

The model is **recovered from the stock firmware, not guessed**:

```
R    = 56000 * V / (3300 - V)          ; 56k series, thermistor low side
1/T  = 1/ntc_t0 + (1/ntc_beta0) * ln(R / ntc_r0)
```

The stock firmware fetches those three via `nonvolatile_get_float()` as NVS
keys `ntc_beta0` / `ntc_t0` / `ntc_r0` (float key indices 5, 6, 7), falling
back to a defaults table at `0x3f40e0c4`:

| key | default |
|---|---|
| `ntc_beta0` | 3950 |
| `ntc_t0` | 298.15 K |
| `ntc_r0` | 100000 Ω |

A 100 kΩ NTC, β=3950, referenced at 25 °C. Those defaults are what
`board.h` now uses.

**Expect a residual error on an uncommissioned board.** A factory unit also
carries per-unit correction in NVS — `cal_temp0`/`cal_adc0`,
`cal_temp1`/`cal_adc1` and `temp_offset` — which absorb ADC and thermistor
tolerance. A brand-new mainboard (`registered = 0`, no `cal_*` keys) has none
written, so it reads several degrees low. Stock firmware on the same board
would read the same.

### Calibrating the internal probe

The same idea as the stock `cal_*` keys, and persisted in NVS so it survives
reboots and OTA pushes.

`celsius_raw` (straight off the beta curve) and `celsius` (after correction)
are both reported, so the correction is never invisible.

| points captured | behaviour |
|---|---|
| none | raw curve, plus any manual `offset` |
| one | constant offset — the error at that temperature |
| two, ≥5 °C apart | linear fit through both, extrapolated beyond them |

Two points closer than 5 °C are rejected as a slope and fall back to an
offset: fitting a line through two nearly-identical readings turns sensor
noise into a wildly wrong gradient.

**A RAPT Pill makes an excellent reference** — it is factory-calibrated, so
`from=pill` captures against it directly. Put the probe and the Pill in the
same place and let both settle first; calibrating against a sensor sitting
somewhere else just bakes in the difference between two locations.

```bash
curl -X POST "http://<ip>/api/calibrate?point=0&from=pill&save=1"
curl -X POST "http://<ip>/api/calibrate?point=1&actual=4.0&save=1"
curl -X POST "http://<ip>/api/calibrate?clear=1"
```

| parameter | notes |
|---|---|
| `point` | `0` or `1` |
| `actual` | the true temperature right now |
| `from=pill` | use the current Pill reading instead of `actual`; refuses if older than 15 min |
| `offset` | constant trim, independent of the two-point pair |
| `clear` | forget all calibration |
| `save` | persist to NVS |

There are buttons for this in the web UI's probe card.

## Compressor protection

`relays.c` enforces interlocks that the raw GPIO layer does not:

- heater and cooler can never be energised together
- minimum off time before the compressor restarts (default 300 s)
- minimum run time before it stops (default 120 s)
- dwell when switching between heating and cooling (default 60 s)

These correspond to the stock `compressorDelay` and `modeSwitchDelay` device
twin properties. A refused request returns `ESP_ERR_INVALID_STATE` rather than
being silently queued, so the caller decides what to do. Keep them. A
compressor restarted against head pressure can stall and burn out.

`relays_emergency_stop()` bypasses the minimum run time — use it for probe
faults and shutdown, not for normal control.

## Not yet built

- Per-unit temperature correction — `cal_temp0`/`cal_adc0`,
  `cal_temp1`/`cal_adc1`, `temp_offset` (see above). The Pill is
  factory-calibrated and reads correctly, so it makes a convenient reference
  to calibrate the wired probe against.
- The second probe channel (`ntc_beta1`/`ntc_t1`/`ntc_r1`)
- Fermentation profiles — the stock firmware fetches stepped profiles from
  the cloud and walks them over time
- KegLand cloud client — OAuth device flow, then Azure IoT Hub over MQTT.
  Do not vendor `esp-azure`; derive a SAS token from the `connectionString`
  and use `esp-mqtt` directly.
- LVGL UI on the panel — currently the display is only proven with a fill
- RAPT Pill / RAPT Temp BLE scanning (NimBLE observer; sdkconfig is ready)
- KegLand cloud client — OAuth device flow, then Azure IoT Hub over MQTT.
  Do not vendor `esp-azure`; derive a SAS token from the `connectionString`
  and use `esp-mqtt` directly.
- LVGL UI

## TLS note

Do not copy the stock firmware's CA bundle. It is a fixed four-root store and
the pinned Baltimore CyberTrust Root **expired 2025-05-12**. Build a current
bundle instead.
