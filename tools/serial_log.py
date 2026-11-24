#!/usr/bin/env python3
"""Reads the device serial log for N seconds (optionally resetting it first). Usage: serial_log.py COM3 [seconds] [--reset]"""
import sys, time, serial
port = sys.argv[1]; secs = float(sys.argv[2]) if len(sys.argv) > 2 and not sys.argv[2].startswith("--") else 20
s = serial.Serial(port, 115200, timeout=0.2)
if "--reset" in sys.argv:
    s.dtr = False; s.rts = True; time.sleep(0.1); s.rts = False; time.sleep(0.1)
end = time.time() + secs
buf = b""
while time.time() < end:
    d = s.read(512)
    if d: sys.stdout.write(d.decode("utf-8", "replace")); sys.stdout.flush()
s.close()
