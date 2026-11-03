# RAPT BLE advertisement framing (all devices)

Every RAPT device broadcasts its telemetry as BLE **manufacturer-specific data**.
Libraries such as `bleak` key this by company id; RAPT's is **`0x4152`**. On air the
company bytes are `52 41` ("RA") and the value itself begins `50 54` ("PT"), so
together the raw AD structure spells **R A P T**.

```
value[0:2]   "PT"              0x50 0x54
value[2]     version byte      selects the format (table below)
value[3:9]   device MAC        6 bytes, big-endian  (0xA1 / 0xB1 formats)
value[9:]    payload           format-specific
```

> The Pill's `0x01`/`0x02` formats put different fields after the version byte
> (temperature/gravity/tilt/battery, big-endian) — see `pill/README.md` and the
> public `rapt-ble` parser. The MAC-at-`[3:9]` layout above is the newer family.

## Version byte → format

| version | format | where documented |
|---|---|---|
| `0x01` | Pill v1 plaintext (MAC + telemetry) | `pill/README.md` |
| `0x02` | Pill v2 plaintext (velocity + telemetry) | `pill/README.md`, `rapt-ble` |
| `0xA1` | **Laser level, plaintext** | `laserLevel/laser-ble-protocol.md` |
| `0xB1` | **Encrypted** — AES-128 keystream keyed by the device's `thingName` | `common/encrypted-advert-0xB1.md` |
| `0xB7` | Pill 2.0.0 encrypted | undecoded; expected to be the `0xB1` family |

Rule of thumb: high nibble `0xB` = encrypted, `0xA`/`0x0` = plaintext.

## Identifying which device you are hearing

Match on the **embedded MAC at `value[3:9]`**, not on the BLE advertising address.
The advertising address can differ from the embedded MAC (observed: +2 on the last
byte on one device, equal on another). The portal lists each device's registered
`macAddress`, which corresponds to the embedded MAC.

## Scanning (Windows / Python)

```python
from bleak import BleakScanner
CID = 0x4152
def cb(dev, adv):
    v = bytes((adv.manufacturer_data or {}).get(CID, b""))
    if len(v) >= 9 and v[:2] == b"PT":
        print(dev.address, "ver=0x%02x" % v[2], "mac=" + v[3:9].hex(), v.hex())
```

`laserLevel/rapt_laser_decode.py` is a complete scanner that dispatches on the
version byte.
