# The 0xB1 encrypted advert — how RAPT devices encrypt their beacons

Recovered from the RAPT Brew Android app (`io.rapt.app`, 1.1.5), a Flutter app whose
logic lives in the Dart AOT snapshot `libapp.so`. Decompiled with
[blutter](https://github.com/worawit/blutter) (Dart 3.10.7, arm64). The decode
function is `_DeviceDetailShellState::_decodeLaserB1Payload` in
`lib/features/devices/ui/detail_shell/device_detail_shell.dart`.

**Verified** against live captures: a FermChamber's held-still adverts all decrypt
to the same bytes, which only happens with the right key.

## The scheme

```
key    = SHA256( utf8(thingName) )[:16]        # AES-128 key — first half of the hash
blob   = value[9:]                             # 10-byte payload after "PT"+0xB1+MAC
nonce  = blob[0:4]                             # per-packet random prefix
ks     = AES_128_ECB_encrypt( key, nonce + 12×0x00 )   # one block = 16-byte keystream
plain  = blob[4:] XOR ks[:6]                   # 6 decrypted bytes
```

```python
import hashlib
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

def decode_b1(value: bytes, thing_name: str) -> bytes:
    key   = hashlib.sha256(thing_name.encode()).digest()[:16]
    blob  = value[9:]
    if len(blob) < 10:
        return b""
    nonce = blob[0:4]
    enc   = Cipher(algorithms.AES(key), modes.ECB()).encryptor()
    ks    = enc.update((bytes(nonce) + b"\x00" * 16)[:16]) + enc.finalize()
    return bytes(c ^ ks[i] for i, c in enumerate(blob[4:]))
```

This is AES-ECB used purely as a keystream generator: the random nonce changes the
keystream every packet, which is why two adverts at the same reading share no bytes.

## The key: `thingName`

The key is derived from the device's **AWS IoT `thingName`**, format
`<DeviceType>-<uuid>`, e.g. `FermChamber-<uuid>` or `LaserLevelSensor-<uuid>`.
It is a per-device, per-account cloud identifier and is **not** in the advert and
**not** derivable from the MAC — that is the beacon's actual security.

The app fetches it from the device **twin** (GraphQL `get_device_twin`, field
`thingName`; falls back to `thing_name`/`thingname`). Without it the app logs
*"thing_name unavailable; cannot decode yet"*.

### Getting yours
1. Sign in at `https://brew.rapt.io` and open the device's detail page
   (`/devices/<type>/<mac>_HASH<id>`). The `/devices` list does **not** include it.
2. The `thingName` is in the page's RSC payload — search the detail `?_rsc=` network
   response, or run in the console:
   `document.documentElement.outerHTML.match(/"thingName":"([^"]+)"/g)`.

**Treat it as a secret.** Anyone with it can decrypt that device's beacons in BLE range.

## What the 6 decrypted bytes mean

Per device type. Observed so far:
- **FermChamber (FridgeG5):** the 6 bytes are **ASCII hex text encoding 3 binary
  bytes**; the first two are the **chamber temperature as a big-endian signed 16-bit
  8.8 fixed-point value (÷256 → °C, 1/16 °C resolution)**, the third byte has been
  `0x00` in every packet (status/flags or padding). E.g. `"056000"` → `05 60 00` →
  `0x0560 / 256 = +5.375 °C`. Verified live against the portal and a door-open test
  (see `fermentationChamber/README.md`). The text encoding is characteristic of the
  older `fancyfridge` firmware.
- **AirStill:** the app parses its decrypted payload as raw bytes — `[0:2]` BE ÷ 10 =
  temperature °C, `[2:4]` BE ÷ 10 = ethanol % (`_onLiveScanDevice`, "AirStill live").
- **Laser level:** this device type uses the plaintext `0xA1` advert on the tested
  unit, so its `0xB1` layout is untested.

Note the app's own live scanner only decodes the laser and AirStill beacons; it does
not decode the chamber beacon at all (chamber temperature comes from the cloud), so
the FermChamber layout above is this project's finding, not the app's.

## Verification oracle (no key needed to test a candidate)

Capture several adverts with the device **held in one state**. With the correct key
the decrypted bytes are constant across packets; a wrong key gives ~0 constant bytes.

## Dead ends (so nobody repeats them)

- Hex constants harvested from `libapp.so` that look like keys are **Google Fonts
  asset checksums** (`google_fonts/part_i.g.dart`).
- "Caldera Derived Key"/HKDF strings belong to the **Cognito login** library, not the beacon.
- `calculateXorChecksum` is the glycol-chiller command checksum.
- In the raw disassembly the key copy reads as `sublist(0, 0x20)` — that `0x20` is a
  **tagged Dart Smi** (= 16), hence AES-128, not AES-256.
