#!/usr/bin/env python3
"""
Repeatable bench flash for a RAPT Pill on a pogo-pin jig.

Assumes the jig wires the adapter's reset lines to the chip:
    DTR -> IO0   and   RTS -> EN
so esptool's normal auto-reset enters the download mode by itself
(no holding IO0, no serial wrapper needed).

By default it writes an app image into the ota_0 slot and points otadata at
slot 0 -- the same minimal, nvs-preserving flash we verified by hand. The
Pill keeps its own registration/Wi-Fi (nvs is untouched), so this is safe to
run on any already-registered Pill to just change its firmware.

Usage:
    python flash_pill.py --port COM7 [--image server_PillG1-beta_1.5.0.bin]

Options:
    --clone <full.bin>   instead of app+otadata, write a full 4MB image to 0x0.
                         WARNING: a full clone also writes the source Pill's
                         nvs (its registration, Wi-Fi, token). Only use a clone
                         made from THIS Pill, never one Pill's dump onto another.
"""
import argparse, subprocess, sys, os, struct, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ESPTOOL = [sys.executable, "-m", "esptool"]

def make_otadata_slot0(path):
    seq = 9; state = 0x2  # seq 9 -> slot (9-1)%2 = 0 ; state VALID
    crc = zlib.crc32(struct.pack("<I", seq), 0xffffffff) & 0xffffffff
    entry = struct.pack("<I", seq) + b"\xff" * 20 + struct.pack("<I", state) + struct.pack("<I", crc)
    open(path, "wb").write(entry + b"\xff" * (0x1000 - len(entry)))

def run(args):
    print("+ esptool " + " ".join(args))
    r = subprocess.run(ESPTOOL + args)
    return r.returncode == 0

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", default="460800")
    ap.add_argument("--image", default=os.path.join(HERE, "server_PillG1-beta_1.5.0.bin"))
    ap.add_argument("--clone")
    a = ap.parse_args()

    base = ["--port", a.port, "--baud", a.baud,
            "--before", "default_reset", "--after", "hard_reset"]

    # Confirm the jig is making contact and the chip enters download mode.
    if not run(base + ["chip_id"]):
        print("\nFAILED to reach the chip. Check: pogo pins seated on all 6 pads,"
              "\n3V3 present, DTR->IO0 and RTS->EN wired, correct --port.")
        sys.exit(1)

    if a.clone:
        ok = run(base + ["write_flash", "0x0", a.clone])
    else:
        ota = os.path.join(HERE, "otadata_slot0.bin")
        make_otadata_slot0(ota)
        ok = run(base + ["write_flash", "0x10000", a.image, "0xd000", ota])

    if ok:
        print("\nOK - flashed and reset. The Pill should now boot the new firmware.")
    else:
        print("\nFAILED during write_flash. Nothing was changed in nvs.")
        sys.exit(1)

if __name__ == "__main__":
    main()
