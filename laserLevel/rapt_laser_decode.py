#!/usr/bin/env python3
"""Live-decode RAPT laser / chamber BLE adverts.

- 0xA1 (plaintext laser): distance is byte[9] in mm, read directly.
- 0xB1 (encrypted, e.g. FermChamber): AES-128 keystream keyed by SHA256(thingName)[:16];
  supply thing names via THING_NAMES (payload-MAC -> thingName).

Usage:  python rapt_laser_decode.py [seconds]
Requires: bleak, cryptography
"""
import asyncio, sys, struct, hashlib
from bleak import BleakScanner

CID = 0x4152  # "RA" little-endian; on-air company bytes spell RAPT with the "PT" payload prefix

# Fill in from the brew.rapt.io device-detail payload (field "thingName") to decode 0xB1:
THING_NAMES = {
    # "7c9ebd928f7c": "FermChamber-<uuid>",
    # "c1171186205e": "LaserLevelSensor-<uuid>",
}

def decode_b1(manuf: bytes, thing_name: str) -> bytes:
    """AES-128 keystream XOR; see laser-ble-protocol.md."""
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    key = hashlib.sha256(thing_name.encode()).digest()[:16]
    blob = manuf[9:]
    if len(blob) < 10:
        return b""
    nonce = blob[0:4]
    enc = Cipher(algorithms.AES(key), modes.ECB()).encryptor()
    ks = enc.update((bytes(nonce) + b"\x00" * 16)[:16]) + enc.finalize()
    return bytes(c ^ ks[i] for i, c in enumerate(blob[4:]))

def parse_a1_laser(m: bytes) -> dict:
    """Plaintext 0xA1 laser advert -> telemetry."""
    return {
        "distance_mm": m[9],                                  # near range: raw mm
        "complement": m[11],                                  # 48 - distance
        "battery_pct": struct.unpack("<H", m[14:16])[0] / 10, # u16 LE / 10
        "sequence": struct.unpack("<H", m[16:18])[0],
    }

SECS = int(sys.argv[1]) if len(sys.argv) > 1 else 30
seen = set()

def cb(dev, adv):
    v = bytes((adv.manufacturer_data or {}).get(CID, b""))
    if len(v) < 9 or v[:2] != b"PT":
        return
    ver = v[2]
    mac = v[3:9].hex()
    key = (dev.address, v.hex())
    if key in seen:
        return
    seen.add(key)
    if ver == 0xA1:
        t = parse_a1_laser(v)
        print(f"[A1 laser] {dev.address} mac={mac} dist={t['distance_mm']}mm "
              f"batt={t['battery_pct']}% seq={t['sequence']} rssi={adv.rssi}")
    elif ver == 0xB1:
        tn = THING_NAMES.get(mac)
        if tn:
            p = decode_b1(v, tn)
            print(f"[B1 {mac}] decrypted={p.hex()} ascii={p.decode('latin1')!r} rssi={adv.rssi}")
        else:
            print(f"[B1 {mac}] encrypted; add thingName for {mac} to decode. raw={v.hex()}")
    else:
        print(f"[0x{ver:02x}] {dev.address} mac={mac} raw={v.hex()}")

async def main():
    s = BleakScanner(detection_callback=cb)
    print(f"Scanning {SECS}s for RAPT adverts...")
    await s.start(); await asyncio.sleep(SECS); await s.stop()

if __name__ == "__main__":
    asyncio.run(main())
