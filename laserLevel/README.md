# RAPT Laser Level Sensor

Keg level sensor that measures the distance to the liquid surface and reports it
over BLE. **It advertises in plaintext** — distance, battery and a sequence counter
can be read straight from the advert with no key.

- [`laser-ble-protocol.md`](laser-ble-protocol.md) — the `0xA1` advert byte layout
  (verified against the brew.rapt.io reading), plus the encrypted `0xB1` family for
  reference.
- [`rapt_laser_decode.py`](rapt_laser_decode.py) — live scanner: prints distance /
  battery / sequence as the sensor advertises. `python rapt_laser_decode.py 30`
  (needs `bleak`, `cryptography`).
- [`HANDOFF-laser-re.md`](HANDOFF-laser-re.md) — full working notes from the
  reverse-engineering effort: how the formats were identified, the app decompilation,
  dead ends, and open items.

## The advert in one table (`version = 0xA1`, 19 bytes)

```
[0:2]   "PT"
[2]     0xA1
[3:9]   MAC
[9]     distance, mm (near range, raw)        0x18 = 24 mm
[10]    0x00
[11]    48 - distance
[12]    mode (0x02)
[13]    0x0A
[14:16] battery  u16 LE / 10  -> %            0x03E8 -> 100.0 %
[16:18] sequence counter u16 LE (+25 per reading)
[18]    0x01
```

The sensor only advertises when its telemetry button is pressed. Identify it by the
embedded MAC at `[3:9]`, which matches the `macAddress` shown on the portal.

## Open items

- Far-range readings: confirm whether `byte[9]` stays raw mm or switches to the
  legacy `801.75 − 3.6119·byte[9]` formula at larger distances.
- Meaning of `[12]`, `[13]`, and whether a temperature field exists in the advert.
