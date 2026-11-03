# RAPT BLE adverts — plaintext laser (0xA1) and encrypted (0xB1)

> **Key finding (2026-11-03):** the **Laser Level Sensor advertises in PLAINTEXT (0xA1)** —
> distance is `byte[9]` in mm, read directly, no key needed. The **encrypted 0xB1** scheme
> is used by *other* devices on this account (the **FermChamber "Fridge"**). The earlier
> sessions were sniffing the Fridge (BLE `7C:9E:BD:92:8F:7E`, payload MAC `7c9ebd928f7c`),
> not the laser (BLE `C1:17:11:86:20:5E`, payload MAC `c1171186205e`).

## 0xA1 — plaintext laser advert (19 bytes)

```
offset bytes meaning
0      2     "PT"
2      1     0xA1 (plaintext laser)
3      6     MAC (c1 17 11 86 20 5e)
9      1     distance in mm (near range; raw value)   <-- e.g. 0x18 = 24 mm
10     1     0x00
11     1     48 - distance (complement / range check)
12     1     mode (0x02)
13     1     0x0A constant
14     2     battery, u16 LE / 10  -> % (0x03E8 = 1000 -> 100.0%)
16     2     sequence counter, u16 LE (steps of 25 per reading)
18     1     0x01
```
Verified live: held at ~24 mm the sensor reported `byte[9]` = 0x18/0x19 (24/25 mm),
matching the brew.rapt.io "Laser reading 24 mm" / `rawReading: 24`.
Decoder: `rapt_laser_decode.py`.

---

# Encrypted BLE advert (version 0xB1)

Reverse-engineered from the RAPT Brew Android app (`io.rapt.app`, RAPT-Brew 1.1.5),
Flutter/Dart AOT snapshot `libapp.so`, decompiled with **blutter** (Dart 3.10.7,
snapshot `1ce86630892e2dca9a8543fdb8ed8e22`, arm64-v8a).

Decode function: `_DeviceDetailShellState::_decodeLaserB1Payload`
(`lib/features/devices/ui/detail_shell/device_detail_shell.dart`), called from
`_onLiveScanDevice` when the manufacturer-data version byte is `0xB1`.

## Advertisement framing (manufacturer data, company id 0x4152 = "RA" → on-air "RAPT")

```
offset  bytes  meaning
0       2      "PT"            (0x50 0x54) — with the company id this spells R A P T
2       1      version         0xB1 = encrypted laser   (0xA1 = plaintext laser, legacy)
3       6      MAC             device MAC, big-endian (e.g. 7C 9E BD 92 8F 7C)
9       10     encrypted blob  <-- this 10-byte tail is what gets decoded
```

So a full 0xB1 advert manufacturer value is 19 bytes, e.g.
`5054 b1 7c9ebd928f7c 4810173bca64807255d4`.

## Decode algorithm (exact — CONFIRMED against live captures)

```python
import hashlib
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

def decode_rapt_b1(manuf: bytes, thing_name: str) -> bytes:
    # key = first 16 bytes of SHA-256(utf8(thing_name))  -> AES-128 key
    key = hashlib.sha256(thing_name.encode("utf-8")).digest()[:16]

    blob  = manuf[9:]            # strip "PT"+version+MAC (9-byte header)
    if len(blob) < 10:           # app requires length >= 10
        return b""
    nonce = blob[0:4]            # per-packet random prefix -> keystream varies every packet
    ct    = blob[4:]             # 6 bytes of ciphertext

    # keystream = AES-128-ECB( key, nonce || 12 zero bytes )   (single 16-byte block)
    block = (bytes(nonce) + b"\x00" * 16)[:16]
    enc = Cipher(algorithms.AES(key), modes.ECB()).encryptor()
    ks  = enc.update(block) + enc.finalize()          # 16 bytes

    return bytes(c ^ ks[i] for i, c in enumerate(ct)) # 6 decrypted bytes
```

### CONFIRMED 2026-11-03
- **`thingName` format** = `<DeviceType>-<uuid>`, fetched from the device twin
  (field `thingName`, visible in the brew.rapt.io device-detail RSC payload; NOT in the
  `/devices` list). Examples from this account:
  - FermChamber "Fridge": `FermChamber-<uuid>`
  - Laser Sensor:          `LaserLevelSensor-<uuid>`
- **AES-128, not 256.** The key is `SHA256(thingName)[:16]`. (The Dart `sublist(0, 0x20)`
  is a *tagged* Smi: 0x20 = 16, so the key copy is 16 bytes. Same tagging explains the
  4-byte nonce and 6-byte ciphertext slices.)
- **Proof:** 11 held-still FermChamber adverts all decrypt to the identical 6 bytes
  `66 66 33 30 30 30` = ASCII `"ff3000"` (a colour hex). Per-packet nonces differ, so a
  correct constant result is only possible with the right key. Wrong keys → garbage.
- The `0xB1` scheme is **shared across RAPT device types** (FermChamber, laser, …); each
  device's beacon is keyed by *its own* `thingName`. The decrypted 6-byte payload layout is
  per-device-type (colour string for the chamber; distance/telemetry for the laser).

### Confirmed details from the decompiled code
- **Key**: `SHA256(utf8(this.field_83))`. `field_83` is set from the device **`thing_name`**
  (the AWS-IoT thing name), fetched by the app via the GraphQL `get_device_twin` query or
  read from the device-detail record. Debug strings that pin this down:
  - `✅ [LIVE_BEACON] Got thing_name from device detail: `
  - `✅ [LIVE_BEACON] Loaded thing_name from twin for `
  - `⚠️ [LIVE_BEACON] thing_name unavailable from get_device_twin … keeping raw payload logging only`
- **Cipher**: pointycastle `ECBBlockCipher(AESEngine())`, `init(forEncryption=true, KeyParameter(key))`,
  one `processBlock`. The app never uses CTR/CBC/stream modes — ECB of the nonce block is
  the keystream.
- **XOR**: explicit byte loop (`eor`) of ciphertext against the keystream.
- **counter**: after decode the caller reads `plain[0] | plain[1] << 8` and logs
  `🔐 [LIVE_BEACON] Laser B1 decode counter=` — so `plain[0:2]` is a little-endian packet
  counter (increments per advert at a fixed distance); `plain[2:]` carries the telemetry.

### Why earlier brute force failed (all ruled out)
- The 9 "key" hex constants harvested from `libapp.so` were **Google Fonts asset SHA-256
  checksums** (`google_fonts/part_i.g.dart`), not crypto keys.
- "Caldera Derived Key" / HKDF is the **Cognito** login crypto, unrelated to the beacon.
- The real key is `SHA256(thing_name)` — a per-device cloud secret, absent from the advert
  and not derivable from the MAC. This is the actual security of the beacon.

## The one missing input: `thing_name`

Decoding any specific unit requires that unit's `thing_name`. It is **not** the MAC
(verified: no MAC/any-case/colon/prefix variant decrypts the held-still capture). Get it from
the RAPT cloud for the signed-in account, e.g.:
- the RAPT web portal device details (app.rapt.io), or
- the RAPT API device/telemetry endpoints (returns the device id / thing name), or
- capture the app's `get_device_twin` GraphQL response.

## Verification oracle (no thing_name needed to test a candidate)
Capture several adverts **at a fixed distance**. With the correct `thing_name`,
`plain[2:]` is constant across packets while `plain[0:2]` (the counter) increments.
A wrong key yields ~0 constant bytes. Script: `scratchpad/verify_laserb1.py`.

## Decrypted-field layout (to confirm once a real thing_name is in hand)
`plain` is 6 bytes: `[0:2]` = counter (LE). The remaining 4 bytes carry distance /
temperature / battery, matching the legacy **0xA1 plaintext** field math (see the
thewolf-oz/rapt-laser-level layout and `deriveMmFromX10`: distance_mm = raw / 10).
Exact mapping to be pinned down against a known distance/temperature/battery reading.
