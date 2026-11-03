#!/usr/bin/env python3
"""
Dump the Pill's full 4 MB flash.

Hold IO0 to GND, then run this. It pulses EN (via RTS) to reset the chip;
with IO0 low at reset the ROM enters UART download mode, then it connects
(RTS/DTR released so the open doesn't hold EN low) and reads the flash.

    python pill_dump.py <outfile.bin>
"""
import sys, time, serial, esptool

PORT = "COM7"
out = sys.argv[1] if len(sys.argv) > 1 else "pill_dump.bin"

s = serial.Serial()
s.port = PORT; s.baudrate = 115200
s.rts = False; s.dtr = False
s.open()

# Pulse EN low->high to reset. IO0 (held low by hand) latches download mode.
s.rts = True; time.sleep(0.3); s.rts = False
time.sleep(0.2)

esp = esptool.ESP32ROM(s, 115200)
esp.connect("no_reset")
esptool.main(
    ["--before", "no_reset", "--after", "no_reset",
     "--baud", "460800",
     "read_flash", "0", "0x400000", out],
    esp=esp,
)
