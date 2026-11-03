# RAPT Pill 2.0.0 — BLE command protocol (reverse‑engineered)

Reversed from `app_2.0.0.bin` with Ghidra (PyGhidra). Function addresses refer
to that image. The device BLE‑advertises as **"RAPT Pill Hydrometer"**, BLE MAC
`c4:5b:be:b2:be:c2` (base MAC `…:c0` + 2).

## Advertisement telemetry (no connection needed)

Manufacturer data, company id `0x4152` ("RA" → on‑air `RAPT…`). The device emits
the standard **v2 metrics** packet (version byte `0x02`) decodable by the public
`rapt-ble` format — temperature, gravity, x/y/z tilt, battery — plus a newer
2.0.0 packet (version byte `0xb7`) not yet decoded. `pill_ble.py scan` prints both.

## GATT services

Three custom services, base UUID `52415054-0a06-9215-08bf-f2b01884__00`
(`52415054` = "RAPT"):

| Service | Characteristics |
|---|---|
| `…840000` | `0004/0005/0006` rw, `0007` write |
| `…840100` | `0101/0102` rw, `0103` write, `0104` rw+notify, `0105` read+notify |
| `…840200` | `0201` rw, `0202/0203` write |

All custom characteristics require a **bonded (paired) connection** — reads/writes
on an unpaired link return `Insufficient Authentication`. Pair once (`client.pair()`;
the bond persists in the OS).

## Command channel

The "devinfo" service write handler is `FUN_400ee060(sub_id, buf, len)`
(registered as the write callback at `0x400ee060`). It dispatches on an internal
**sub‑characteristic id**:

- `0x0C` — **owner authentication** (`FUN_400ec740`).
- `0x0E` — **command**: requires owner‑auth (see below), then parses `buf` as a
  **JSON object** (BOM tolerated) and reads the string field **`cmd`** and optional
  **`args`**, calling the dispatcher `FUN_400e63e8(cmd, args)`.
- `0x0B` — **"desired"**: JSON `{"desired": …}`, applied to the AWS shadow desired state.

**CONFIRMED empirically (via the ESP32 central):** the single control
characteristic is **`52415054-0a06-9215-08bf-f2b018840006`** (value handle `0x14`).
A 2×2 accept/reject probe across the service‑0000 chars (Pill power‑cycled,
owner‑auth off) showed that **only `…0006` accepts JSON control, and it accepts
both `{"cmd":…}` and `{"desired":{…}}`** — so commands *and* settings go to the
same characteristic, routed by the top‑level key. `…0004`, `…0005`, `…0007`
reject both. (The decompiled service callback that suggested separate
`desired`/`auth` chars was mis‑analysed by Ghidra — the live device is
authoritative.) Owner‑auth is not a separately reachable char here and is moot
without the session secret.

| write to `…0006` | result |
|---|---|
| `{"cmd":"report_now"}` | ACCEPTED (link held) |
| `{"desired":{}}` | ACCEPTED |
| anything to `…0004/0005/0007` | REJECTED |

So a command is:

```json
{"cmd":"report_now"}
{"cmd":"keep_awake"}
{"cmd":"start_calibration_wort","args":{"gravity":1.057}}
{"cmd":"factory_reset","args":{"mac":"c4:5b:be:b2:be:c0"}}
```

### Command set (dispatcher `FUN_400e63e8`)

| cmd | args | effect |
|---|---|---|
| `keep_awake` | — | stay awake ~120 s (a gesture session) |
| `report_now` | — | send a telemetry report now (refused if not registered) |
| `upload_log` | — | upload the log |
| `start_automation` / `stop_automation` | name | start/stop a fermentation automation |
| `start_calibration_water` / `start_calibration_wort` | `gravity` (number) | calibration points |
| `cancel_calibration` / `clear_calibration` | — | calibration control |
| `set_dev_options` | `dev_options` | toggle developer options |
| `set_beta_updates` | `beta_updates`, `check` | beta channel |
| `clear_wifi` | — | forget saved Wi‑Fi (refused during update/calibration) |
| `clear_registration` | — | clear registration |
| `factory_reset` | `mac` = **this device's own MAC** | erase NVS + restart; refused if mac missing/mismatched |
| `export` | `what` ∈ {gesture_log, gravity_data, log}, `page` | export data |

Unknown names → `unknown command "<s>"`.

## Owner‑auth gate — the catch

Guarded by a **runtime flag** `*DAT_400d2468` and a **secret** at `DAT_400d2484`:

- `FUN_400ec720` (is‑authenticated): **if the flag is 0, it returns TRUE
  unconditionally** — no auth required. Only when the flag is 1 must the peer have
  authenticated this connection.
- `FUN_400ec740` (authenticate, sub‑char `0x0C`): compares the written bytes to the
  stored secret (`memcmp`, trailing whitespace trimmed); on match sets this
  connection authenticated ("Unlocked by the peer"); 5 wrong tries → ~3 s lockout
  ("Wrong secret from the peer").
- `FUN_400ed634(secret)` sets it: non‑empty → `strcpy` secret + flag = 1 (enable);
  empty/NULL → zero secret + flag = 0 (disable).

Crucially the **flag and secret live only in RAM** (`0x3ffc5f80`/`0x3ffc5f81`,
BSS), are **zeroed at boot**, and have **no NVS copy** (confirmed by parsing the
dump's NVS — only `refresh_tok`, `aws_cert`, `aws_key`, Wi‑Fi, calibration, etc.).
So owner‑auth starts **disabled after every reboot** and is only enabled when a
session provisions a secret (via the BLE control loop `FUN_400ec27c`, tied to the
RAPT Brew app / cloud handshake). The secret is **not recoverable from our dump**.

### Practical consequence

- To send `report_now`/`keep_awake` without the secret, command the device while
  owner‑auth is still **disabled** — i.e. shortly after a reboot, before the app/cloud
  provisions a secret.
- If owner‑auth is already enabled, you need that session secret (held by the RAPT
  Brew app / cloud); it is not in the firmware or NVS.

## Tooling

`pill_ble.py`:
- `scan` — advertisement telemetry (works today, no pairing).
- `gatt` — map services/characteristics (bonded).
- `probe` — pair + read all characteristics + subscribe notifications.
- `send <cmd>` — pair, write `{"cmd":"<cmd>"}` to the command characteristic
  (probes the writable chars), safe names only unless `--force`.

### Known limitation (this machine)

Windows/WinRT connects to this Pill are unreliable — it accepts GATT connections
only in brief connectable windows, and `connect()` sometimes fails with empty
errors or stalls (now hard‑capped at 22 s/attempt). The dependable alternatives
for the command channel are a **Linux/bluez host** (bleak is far more reliable
there) or the **RAPT Brew app**. Telemetry via `scan` is unaffected.
