#!/usr/bin/env python3
"""
Discover the BLE framing for the Pill's text "Direct commands" by sending ONLY
the benign `report_now` and watching the 921600 serial console as an oracle.

Safe: report_now just triggers a telemetry report; any wrong framing is logged
and refused by the firmware. No destructive command is ever sent.

Needs: bonded BLE (pair once with pill_ble.py probe) + serial jig on COM7.
"""
import asyncio, threading, time, re, serial
from bleak import BleakScanner, BleakClient

ADDR = "C4:5B:BE:B2:BE:C2"
BASE = "52415054-0a06-9215-08bf-f2b01884"   # + 4 hex nibbles
WRITE_CHARS = ["0007", "0103", "0202", "0203", "0005", "0102"]
CMD = b"report_now"

serial_lines = []
stop = False
def reader():
    try:
        s = serial.Serial("COM7", 921600, timeout=0.1); s.rts = False; s.dtr = False
    except Exception as e:
        serial_lines.append("(serial open failed: %s)" % e); return
    buf = b""
    while not stop:
        buf += s.read(4096)
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            txt = re.sub(r"\x1b\[[0-9;]*m", "", line.decode("latin1")).rstrip("\r")
            if txt:
                serial_lines.append(txt)
    s.close()

def snapshot(since):
    return serial_lines[since:]

async def main():
    th = threading.Thread(target=reader, daemon=True); th.start()
    time.sleep(0.5)
    print("scan+connect...", flush=True)
    await BleakScanner.discover(timeout=8)
    c = BleakClient(ADDR, timeout=20)
    await c.connect()
    print("CONNECTED", flush=True)
    await asyncio.sleep(2)
    base = len(serial_lines)
    print("serial during connect:", snapshot(0)[-8:], flush=True)
    try:
        for suf in WRITE_CHARS:
            uuid = BASE + suf
            before = len(serial_lines)
            try:
                await c.write_gatt_char(uuid, CMD, response=True)
                wrote = "ok"
            except Exception as e:
                wrote = "write err: %s" % e
            await asyncio.sleep(2.5)
            new = snapshot(before)
            print("\n== char %s  write(%s)=%s ==" % (suf, CMD.decode(), wrote), flush=True)
            for ln in new:
                if re.search(r"command|Direct|report|refus|unknown|GATT|sub-char", ln, re.I):
                    print("   SERIAL:", ln, flush=True)
            if not new:
                print("   (no serial output)", flush=True)
    finally:
        await c.disconnect()
    global stop; stop = True
    time.sleep(0.3)

asyncio.run(main())
