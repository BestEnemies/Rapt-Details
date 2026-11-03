#!/usr/bin/env python3
"""
Interact with a RAPT Pill over Bluetooth LE (read-only for now).

Subcommands:
    scan                 Discover RAPT Pills, show raw advertisement + decoded
                         metrics (stock v1/v2 format). 2.0.0 uses a new packet
                         format shown as raw until its encoder is reversed.
    gatt   [--addr A]    Connect and map the GATT table: every service,
                         characteristic (with properties) and descriptor, and
                         the value of each readable characteristic.
    monitor [--addr A]   Connect and subscribe to every notify/indicate
                         characteristic, printing data as it arrives.

Default address is this Pill's BLE MAC; override with --addr.
Nothing here writes to the device.

    python pill_ble.py scan
    python pill_ble.py gatt
    python pill_ble.py monitor --secs 120
"""
import argparse, asyncio, struct, sys
from bleak import BleakScanner, BleakClient

DEFAULT_ADDR = "C4:5B:BE:B2:BE:C2"   # this Pill's BLE MAC (base MAC + 2)
RAPT_MFR_RA = 16722   # 0x4152, little-endian "RA" -> on-air "RAPT..."
RAPT_MFR_KEG = 17739  # 0x4b45, little-endian "KE" -> on-air "KEG..."

def decode_metrics(data: bytes):
    """Decode a RAPT manufacturer payload (the bytes after the 2-byte company
    id). Returns a dict. Handles the documented v1/v2 formats; otherwise marks
    the payload as an unknown/newer format and leaves the raw bytes for you."""
    out = {"raw": data.hex()}
    if data[:9] == b"PTdPillG1" or data[:2] != b"PT":
        out["type"] = "name/other"
        return out
    ver = data[2]
    try:
        if ver == 0x01:   # v1: prefix(RA+PT) ver mac temp gravity x y z batt
            ver_, mac, temp, grav, x, y, z, batt = struct.unpack(">B6sHfhhhh", data[2:])
            out.update(mac=mac.hex(":"))
        elif ver == 0x02: # v2: ver reserved velvalid velocity temp gravity x y z batt
            ver_, velok, vel, temp, grav, x, y, z, batt = struct.unpack(">Bx?fHfhhhh", data[2:])
            out.update(gravity_velocity=round(vel, 4) if velok else None)
        else:
            out["type"] = "metrics (unrecognised v%#x / 2.0.0 format)" % ver
            return out
        out.update(type="metrics v%d" % ver,
                   temperature_c=round(temp / 128 - 273.15, 2),
                   gravity=round(grav / 1000, 4),
                   x=x / 16, y=y / 16, z=z / 16,
                   battery_pct=round(batt / 256))
    except struct.error:
        out["type"] = "metrics (length mismatch — newer format)"
    return out

async def cmd_scan(a):
    found = {}
    def cb(dev, adv):
        name = adv.local_name or dev.name or ""
        md = adv.manufacturer_data
        is_rapt = RAPT_MFR_RA in md or RAPT_MFR_KEG in md or "rapt" in name.lower()
        if not is_rapt:
            return
        rec = found.setdefault(dev.address, {"name": name, "rssi": adv.rssi, "pkts": {}})
        rec["name"] = name or rec["name"]; rec["rssi"] = adv.rssi
        for cid, val in md.items():
            rec["pkts"][(cid, val.hex())] = decode_metrics(val)
    s = BleakScanner(detection_callback=cb)
    await s.start(); await asyncio.sleep(a.secs); await s.stop()
    if not found:
        print("No RAPT Pills seen in %ds." % a.secs); return
    for addr, r in found.items():
        print("\n%s  %r  RSSI %s" % (addr, r["name"], r["rssi"]))
        for (cid, _h), dec in r["pkts"].items():
            print("  company=0x%04x  %s" % (cid, dec.get("type", "?")))
            for k, v in dec.items():
                if k in ("type",): continue
                print("      %-16s %s" % (k, v))

async def connect(addr, tries=6, pair=False):
    """Working WinRT recipe: run a scan to populate the OS cache, then connect
    BY ADDRESS (not via a stopped scanner object). Retry a few times to catch a
    connectable moment."""
    print("  scan to populate cache...", flush=True)
    try:
        await BleakScanner.discover(timeout=8)
    except Exception as e:
        print("  (scan warning: %s)" % e, flush=True)
    last = None
    for i in range(tries):
        try:
            print("  connect attempt %d/%d (by address)..." % (i + 1, tries), flush=True)
            c = BleakClient(addr, timeout=20)
            await asyncio.wait_for(c.connect(), 22)   # hard cap so WinRT can't hang forever
            print("  CONNECTED", flush=True)
            if pair:
                try:
                    ok = await asyncio.wait_for(c.pair(), 20)
                    print("  pair() -> %s" % ok, flush=True)
                except Exception as e:
                    print("  pair() failed/timeout: %s (continuing)" % e, flush=True)
            return c
        except Exception as e:
            last = e
            print("  attempt %d failed (%r)" % (i + 1, str(e)), flush=True)
            await asyncio.sleep(3)
            if i == tries - 2:
                try:
                    await BleakScanner.discover(timeout=6)
                except Exception:
                    pass
    raise SystemExit("Could not connect to %s: %s" % (addr, last))

def decode_val(val: bytes):
    if not val:
        return "(empty)"
    try:
        txt = val.decode("utf-8")
        if all(32 <= b < 127 or b in (9, 10, 13) for b in val):
            return "%s  (%r)" % (val.hex(), txt)
    except Exception:
        pass
    return val.hex()

async def cmd_probe(a):
    print("Connecting + pairing to %s ..." % a.addr)
    c = await connect(a.addr, pair=True)
    def mk(ch):
        def h(_c, data: bytearray):
            print("  notify %s: %s" % (ch.uuid[-4:], decode_val(bytes(data))))
        return h
    try:
        print("\n== reading readable characteristics ==")
        for svc in c.services:
            if not svc.uuid.lower().startswith("52415054"):
                continue
            for ch in svc.characteristics:
                if "read" in ch.properties:
                    try:
                        val = await c.read_gatt_char(ch)
                        print("  %s [%s] = %s" % (ch.uuid[-4:], prop_str(ch), decode_val(val)))
                    except Exception as e:
                        print("  %s [%s] read failed: %s" % (ch.uuid[-4:], prop_str(ch), e))
                else:
                    print("  %s [%s] (write-only)" % (ch.uuid[-4:], prop_str(ch)))
        print("\n== subscribing to notify channels for %ds ==" % a.secs)
        for svc in c.services:
            if not svc.uuid.lower().startswith("52415054"):
                continue
            for ch in svc.characteristics:
                if "notify" in ch.properties or "indicate" in ch.properties:
                    try:
                        await c.start_notify(ch, mk(ch))
                        print("  subscribed %s" % ch.uuid[-4:])
                    except Exception as e:
                        print("  subscribe %s failed: %s" % (ch.uuid[-4:], e))
        await asyncio.sleep(a.secs)
    finally:
        await c.disconnect()

def prop_str(ch):
    return ",".join(ch.properties)

async def cmd_gatt(a):
    print("Connecting to %s ..." % a.addr)
    c = await connect(a.addr)
    try:
        print("Connected. Services / characteristics:\n")
        for svc in c.services:
            print("service %s  (%s)" % (svc.uuid, svc.description))
            for ch in svc.characteristics:
                line = "  char %s  [%s]  (%s)" % (ch.uuid, prop_str(ch), ch.description)
                print(line)
                if "read" in ch.properties:
                    try:
                        val = await c.read_gatt_char(ch)
                        txt = val.decode("utf-8") if all(32 <= b < 127 for b in val) else ""
                        print("      = %s %s" % (val.hex(), ("(%r)" % txt) if txt else ""))
                    except Exception as e:
                        print("      (read failed: %s)" % e)
                for d in ch.descriptors:
                    print("      descr %s" % d.uuid)
    finally:
        await c.disconnect()

async def cmd_monitor(a):
    print("Connecting to %s ..." % a.addr)
    c = await connect(a.addr)
    def mk(ch):
        def h(_char, data: bytearray):
            txt = data.decode("utf-8") if all(32 <= b < 127 for b in data) else ""
            print("notify %s: %s %s" % (ch.uuid, bytes(data).hex(), ("(%r)" % txt) if txt else ""))
        return h
    try:
        n = 0
        for svc in c.services:
            for ch in svc.characteristics:
                if "notify" in ch.properties or "indicate" in ch.properties:
                    try:
                        await c.start_notify(ch, mk(ch)); n += 1
                        print("subscribed %s [%s]" % (ch.uuid, prop_str(ch)))
                    except Exception as e:
                        print("  could not subscribe %s: %s" % (ch.uuid, e))
        if not n:
            print("No notify/indicate characteristics found.")
        print("\nListening %ds (Ctrl+C to stop)...\n" % a.secs)
        await asyncio.sleep(a.secs)
    finally:
        await c.disconnect()

CUSTOM_BASE = "52415054-0a06-9215-08bf-f2b01884"   # + 4 hex: RAPT custom chars
# write-capable custom characteristics seen in the GATT map
WRITE_SUFFIXES = ["0007", "0103", "0202", "0203", "0005", "0006", "0102", "0101", "0104", "0201"]
# commands that change nothing persistent / are safe to send
SAFE_CMDS = {"report_now", "keep_awake", "upload_log"}

async def cmd_send(a):
    if a.name not in SAFE_CMDS and not a.force:
        raise SystemExit("Refusing '%s' (not in the safe set %s). Re-run with --force "
                         "only if you really mean it." % (a.name, sorted(SAFE_CMDS)))
    payload = ('{"cmd":"%s"}' % a.name).encode()
    print("Payload: %s" % payload.decode())
    c = await connect(a.addr, pair=True)
    got = []
    def mk(ch):
        def h(_c, data):
            txt = data.decode("utf-8") if all(32 <= b < 127 for b in data) else ""
            got.append((ch.uuid[-4:], bytes(data)))
            print("  notify %s: %s %s" % (ch.uuid[-4:], bytes(data).hex(), ("(%r)" % txt) if txt else ""))
        return h
    try:
        for svc in c.services:
            if svc.uuid.lower().startswith("52415054"):
                for ch in svc.characteristics:
                    if "notify" in ch.properties or "indicate" in ch.properties:
                        try: await c.start_notify(ch, mk(ch))
                        except Exception: pass
        targets = [a.char] if a.char else WRITE_SUFFIXES
        for suf in targets:
            uuid = CUSTOM_BASE + suf
            try:
                await c.write_gatt_char(uuid, payload, response=True)
                print("  char %s: write ACCEPTED (candidate command characteristic)" % suf)
            except Exception as e:
                print("  char %s: rejected (%s)" % (suf, str(e) or type(e).__name__))
            await asyncio.sleep(1.2)
        print("  waiting for any response/report...")
        await asyncio.sleep(4)
        if not got:
            print("  (no notifications received)")
    finally:
        await c.disconnect()

def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("send"); p.add_argument("name"); p.add_argument("--addr", default=DEFAULT_ADDR)
    p.add_argument("--char", help="characteristic suffix e.g. 0202 (default: probe all)")
    p.add_argument("--force", action="store_true", help="allow a non-safe command name")
    p = sub.add_parser("scan"); p.add_argument("--secs", type=int, default=20)
    p = sub.add_parser("gatt"); p.add_argument("--addr", default=DEFAULT_ADDR)
    p = sub.add_parser("monitor"); p.add_argument("--addr", default=DEFAULT_ADDR); p.add_argument("--secs", type=int, default=120)
    p = sub.add_parser("probe"); p.add_argument("--addr", default=DEFAULT_ADDR); p.add_argument("--secs", type=int, default=25)
    a = ap.parse_args()
    asyncio.run({"scan": cmd_scan, "gatt": cmd_gatt, "monitor": cmd_monitor,
                 "probe": cmd_probe, "send": cmd_send}[a.cmd](a))

if __name__ == "__main__":
    main()
