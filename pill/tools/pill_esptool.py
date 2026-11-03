# Runs esptool against the Pill on an already-open port with RTS/DTR released,
# so opening the port does not hold EN low (RTS is wired to EN).
import sys, serial, esptool
s = serial.Serial()
s.port = 'COM7'; s.baudrate = 115200
s.rts = False; s.dtr = False
s.open()
esp = esptool.ESP32ROM(s, 115200)
esp.connect('no_reset')
esptool.main(['--before', 'no_reset', '--after', 'no_reset'] + sys.argv[1:], esp=esp)
