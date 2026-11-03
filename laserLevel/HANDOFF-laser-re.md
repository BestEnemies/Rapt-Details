# RAPT laser/BLE reverse-engineering — handoff notes

Status: **COMPLETE.** Both advert formats decoded and verified against live captures.
Written 2026-11-03 for a fresh agent continuing this work. No prior conversation context is
assumed — everything needed is here.

---

## 1. Goal

Decode the telemetry in a RAPT **Laser Level Sensor's** BLE advertisement packets
(distance/level, plus battery etc.). Expanded along the way to also crack RAPT's **encrypted**
`0xB1` advert format.

## 2. TL;DR result

- The **laser advertises in PLAINTEXT** (manufacturer-data version byte `0xA1`). Distance is
  `byte[9]` in millimetres, read directly. No key needed. **This is the answer to the original
  question.**
- RAPT's **encrypted `0xB1`** format (used by other devices on this account, e.g. the
  FermChamber) is also fully solved: an **AES-128 keystream XOR**, keyed by
  `SHA256(utf8(thingName))[:16]`, where `thingName` is a per-device cloud secret.
- Earlier sessions were mistakenly sniffing the **FermChamber "Fridge"** (`0xB1`, encrypted),
  not the laser — which is why months of brute force never worked. The laser was plaintext all
  along.

## 3. The devices on this account (from brew.rapt.io `/devices` + device-detail payloads)

| Name | deviceType | registered macAddress | BLE adv address | payload MAC (bytes[3:9]) | advert ver | thingName |
|---|---|---|---|---|---|---|
| Laser Sensor | LaserLevelSensor | `c1-17-11-86-20-5e_HASH<id>` | `C1:17:11:86:20:5E` | `c1171186205e` | **0xA1 (plaintext)** | `LaserLevelSensor-<uuid>` |
| Fridge | FermChamber | `7c-9e-bd-92-8f-7c_HASH<id>` | `7C:9E:BD:92:8F:7E` | `7c9ebd928f7c` | **0xB1 (encrypted)** | `FermChamber-<uuid>` |
| RAPT Pill 2 | PillHydrometer | `c4-5b-be-b2-bc-c8_HASH<id>` | — | — | — | — |
| Pill C0 | PillHydrometer | `c4-5b-be-b2-be-c0_HASH<id>` | — | — | — | — |

Notes:
- BLE advertising address = payload MAC with **+2 on the last byte** (laser `…5e` adv vs `…5e`
  payload are equal here; fridge adv `…7e` vs payload `…7c` differ by 2). Don't rely on this;
  just match on `bytes[3:9]`.
- The laser is `active:false` and only advertises when its **telemetry button is pressed**.
- `thingName` is **NOT** in the `/devices` list. It IS in the device-detail RSC payload
  (field `"thingName"`). See §8 for how to fetch it.

## 4. Advert framing (manufacturer data, BLE company id `0x4152`)

bleak keys `manufacturer_data` by company id `0x4152`. On air the company bytes are `52 41`
("RA") and the value begins `50 54` ("PT") → together they spell **R A P T**. Value layout:

```
offset bytes meaning
0      2     "PT" (0x50 0x54)
2      1     version:  0xA1 plaintext laser | 0xB1 encrypted | (0x01/0x02 plaintext Pill; 0xB7 enc Pill)
3      6     MAC (big-endian)
9      N     payload (plaintext fields for 0xA1; 10-byte encrypted blob for 0xB1)
```

### 4a. 0xA1 plaintext laser payload (19 bytes total) — VERIFIED

```
[9]     distance in mm (near range, raw)        e.g. 0x18 = 24 mm   (matches portal "24 mm")
[10]    0x00
[11]    48 - distance                           (complement / range check)
[12]    mode (0x02)
[13]    0x0A constant
[14:16] battery, u16 LE / 10  -> %              (0x03E8 = 1000 -> 100.0%)
[16:18] sequence counter, u16 LE                (observed stepping +25 per reading)
[18]    0x01
```
Far-range distances (larger raw values) likely use the legacy formula
`mm = 801.75 − 3.6119·byte[9]` from the thewolf-oz/rapt-laser-level fork; **not yet confirmed**
here because the test was all near-range (~24 mm). The app helper `deriveMmFromX10` implies some
paths carry distance ×10; our near-range adverts were raw mm. Confirm by capturing at a large
distance and comparing to the portal reading.

### 4b. 0xB1 encrypted payload — VERIFIED

```python
import hashlib
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

def decode_b1(manuf: bytes, thing_name: str) -> bytes:
    key   = hashlib.sha256(thing_name.encode()).digest()[:16]   # AES-128 (first half of hash)
    blob  = manuf[9:]                                           # 10-byte encrypted blob
    nonce = blob[0:4]                                           # per-packet random -> keystream varies
    enc   = Cipher(algorithms.AES(key), modes.ECB()).encryptor()
    ks    = enc.update((bytes(nonce) + b"\x00"*16)[:16]) + enc.finalize()
    return bytes(c ^ ks[i] for i, c in enumerate(blob[4:]))     # 6 decrypted bytes
```
Decrypted layout is **per device type**. For the FermChamber the 6 bytes are ASCII (observed
`"ff3000"`, a colour hex). The laser never uses `0xB1` on this account, so its `0xB1` field map
is unknown/untested.

## 5. How 0xB1 was recovered (so it can be re-done / extended)

Source: **RAPT Brew Android app** `io.rapt.app`, version RAPT-Brew 1.1.5, user-supplied at
`RAPT Pill/apk/RAPT-Brew_1.1.5_apkcube.apks`. Flutter/Dart AOT — logic is in `libapp.so`
(arm64), a Dart snapshot, **not** in classes.dex.

Tooling: **blutter** (Dart-AOT decompiler) built and run under **WSL Ubuntu 24.04**.
- Dart version resolved from snapshot hash `1ce86630892e2dca9a8543fdb8ed8e22` → **Dart 3.10.7**,
  arm64-v8a.
- REA's `inspect_dart_aot` parses strings/metadata only (no function bodies) — it gave the
  snapshot hash but not the logic. blutter gives the full decompiled assembly + pseudo.

Decode function: `_DeviceDetailShellState::_decodeLaserB1Payload` in
`lib/features/devices/ui/detail_shell/device_detail_shell.dart`, called from `_onLiveScanDevice`
when the version byte is `0xB1`. Key = `SHA256(this.field_83)[:16]`; `field_83` is set from the
device **`thingName`** (debug strings: "Got thing_name from device detail", "Loaded thing_name
from twin", "thing_name unavailable … keeping raw payload logging only"). Cipher is pointycastle
`ECBBlockCipher(AESEngine())`, `init(forEncryption=true)`, one `processBlock`, then byte-wise XOR.

**Gotcha that cost time:** the key length looked like 32 bytes (AES-256) in the raw disassembly
because `sublist(0, 0x20)` uses a **tagged Dart Smi** — `0x20` tagged = decimal **16**. So it's
AES-128 (`[:16]`), and likewise the 4-byte nonce / 6-byte ciphertext slices are tagged. Empirical
verification settled it (see §6).

### Rebuilding the blutter environment (if needed again)
- WSL distro: Ubuntu (the default WSL distro on this machine is kali-linux; **must target
  `-d Ubuntu`**). Ubuntu 24.04 ships gcc 13 which blutter requires.
- One-time deps (needs the user's sudo password — the agent cannot enter it; have the user run):
  ```
  wsl -d Ubuntu -- sudo bash -c "apt-get update && apt-get install -y python3-pyelftools python3-requests git cmake ninja-build build-essential pkg-config libicu-dev libcapstone-dev"
  ```
- Then (sudo-free, agent can drive): clone `https://github.com/worawit/blutter` to `~/blutter`,
  stage `libapp.so`+`libflutter.so` (extract from `apk_x/split_config.arm64_v8a.apk`) into
  `~/rapt/in/`, run `python3 blutter.py ~/rapt/in ~/rapt/out` (first run downloads+builds the
  matching Dart SDK, ~10–30 min). Output: `~/rapt/out/asm/**` (268 MB of Dart assembly),
  `pp.txt`, `objs.txt`, `blutter_frida.js`, `ida_script/`.

### WSL shell gotchas (important — these wasted several calls)
- The outer shell is **Git Bash**. Passing `/mnt/...` paths to `wsl.exe` triggers MSYS path
  rewriting → prefix commands with `MSYS_NO_PATHCONV=1`.
- Inline `wsl.exe -d Ubuntu bash -c '…$VAR…'` **mangles shell variables** (they come back empty)
  through the Git Bash→wsl layer. **Always write a `.sh` file and run it**:
  `MSYS_NO_PATHCONV=1 wsl.exe -d Ubuntu bash "/mnt/c/.../script.sh"`. This is reliable; inline
  multi-var commands are not.

## 6. Verification (how we know it's right)

Oracle: capture many adverts at a **fixed distance**; with the correct key the decrypted bytes
are **constant** across packets (the per-packet nonce varies, so only the right keystream
cancels it). Wrong key → ~0 constant bytes.
- Fridge `0xB1`: 11 held-still packets all decrypt to identical `66 66 33 30 30 30` = ASCII
  `"ff3000"` with `thingName FermChamber-<uuid>`. Definitive.
- Laser `0xA1`: `byte[9]` = 24/25 mm matched the portal "Laser reading 24 mm" / `rawReading:24`.

## 7. Dead ends already ruled out (don't redo these)

- The 9 32-byte hex constants harvested from `libapp.so` are **Google Fonts asset SHA-256
  checksums** (`google_fonts/part_i.g.dart`), not crypto keys.
- "Caldera Derived Key" / HKDF (`computehkdf`, `xorKey`) is the **Cognito login** crypto
  (`amazon_cognito_identity_dart_2`), unrelated to the beacon.
- `calculateXorChecksum` is the **glycol chiller** command-frame checksum, not the laser.
- `thingName` is **not** the MAC or the portal `mac_HASH…` string (tested; no decrypt).
- Blind brute force of hash/HMAC/AES-CTR keystreams never worked because the real key is
  `SHA256(thingName)` — a cloud secret absent from the advert.

## 8. Getting a `thingName` from brew.rapt.io (for 0xB1 devices)

1. Open `https://brew.rapt.io` in the in-app browser; **the user signs in** (never handle their
   password).
2. Open the device's detail page, e.g.
   `https://brew.rapt.io/devices/laser-level-sensor/<mac_HASH>` or `/devices/chamber/<mac_HASH>`.
3. The `thingName` is in the page's RSC flight data. Grab it with:
   `read_network_requests` on the detail `?_rsc=` GET and search for `"thingName":"…"`, or run
   JS: `document.documentElement.outerHTML.match(/"thingName":"([^"]+)"/g)` (chamber pages embed
   it in the initial HTML; the regex `/(FermChamber|LaserLevelSensor|…)-[0-9a-f-]{36}/` also finds it).

## 9. Files

In the project `RAPT Pill/` (open to the user):
- `laser-ble-protocol.md` — full protocol spec, both formats, proofs.
- `rapt_laser_decode.py` — live BLE scanner/decoder. Prints laser distance directly; decodes
  `0xB1` too if you add `mac -> thingName` entries to `THING_NAMES`. Needs `bleak`,`cryptography`.
- `apk/RAPT-Brew_1.1.5_apkcube.apks` — the source APK bundle (UNTRUSTED; static analysis only).
- (`HANDOFF-laser-re.md` — this file.)

In the session scratchpad
`…/31fe9203-…/scratchpad/` (temporary; may be cleared):
- `laser_find_capture.py` — scan all RAPT adverts, group by device, flag the laser. `python laser_find_capture.py <secs>`.
- `laser_scan_all.json` — the laser capture (11 × 0xA1) + fridge (0xB1) from the final scan.
- `laser_set_A.json` — 11 held-still **Fridge** 0xB1 payloads (used to verify the key).
- `verify_laserb1_real.py`, `brute_real.py`, `decode_final.py`, `decode_laser_a1.py`,
  `laser_report.py` — verification/decode scripts (AES-128 confirmed in `brute_real.py`).
- `decomp/` — extracted decompiled functions (`decodeLaserB1.txt`, `resolvers.txt`,
  `field83_stores.txt`, `rapt_advert_matcher.dart`, etc.).
- `apk_x/` — extracted `base.apk`, `split_config.arm64_v8a.apk`, `libapp.so`, `libflutter.so`.
- `wsl_*.sh` — the WSL helper scripts (prep/build/search). Run via
  `MSYS_NO_PATHCONV=1 wsl.exe -d Ubuntu bash "/mnt/c/.../scratchpad/<name>.sh"`.
- Full blutter output lives in WSL at `~/rapt/out/` (not in the Windows tree).

## 10. Open items / possible next steps

1. **Confirm far-range laser distance.** Our capture was all ~24 mm (near range). Capture at a
   larger distance and check whether `byte[9]` stays raw-mm or switches to the
   `801.75 − 3.6119·byte[9]` far-range formula. Also confirm whether any path divides by 10
   (`deriveMmFromX10`).
2. **Map remaining 0xA1 bytes.** `[16:18]` steps +25/reading — confirm it's a sample counter vs a
   timestamp; confirm `[12]`/`[13]` meanings; check for a temperature field (laser may not emit
   one in the advert).
3. **Live demo.** `python rapt_laser_decode.py 30` while moving the sensor shows distance tracking.
4. **0xB1 field maps for other device types** (if ever needed) — decrypt with each device's
   thingName and compare to its portal telemetry.

## 11. Environment facts (this machine)

- Windows 11; outer shell Git Bash; also PowerShell. Python 3.12 on Windows has `bleak` +
  `cryptography` installed and works for BLE scanning.
- WSL: Ubuntu 24.04 (**not** the default distro). blutter built there.
- BLE: the PC's adapter sees all three live RAPT devices; filter by `bytes[3:9]` (payload MAC).
- The RAPT work is a **separate project** from the `BarOS` repo this session's cwd points at;
  keep RAPT artifacts under `Documents/Claude/Projects/RAPT Pill/`.
