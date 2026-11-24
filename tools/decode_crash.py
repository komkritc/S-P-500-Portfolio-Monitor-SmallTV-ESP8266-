#!/usr/bin/env python3
"""Decodes an ESP8266 exception dump (from tools/serial_log.py) against the built ELF.
Usage: decode_crash.py <log.txt> [firmware.elf]"""
import os, re, subprocess, sys
log = open(sys.argv[1], errors="replace").read()
elf = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), "..", ".pio", "build", "smalltv", "firmware.elf")
a2l = os.path.expanduser("~/.platformio/packages/toolchain-xtensa/bin/xtensa-lx106-elf-addr2line.exe")
m = re.search(r"(Exception \(\d+\):[^\n]*\n[^\n]*epc1=[^\n]*)", log)
print(m.group(1) if m else "no 'Exception' header found")
for k in re.findall(r"(?:epc1|epc2|epc3|excvaddr)=0x([0-9a-f]+)", log): 
    print("  addr", k)
addrs = sorted(set(re.findall(r"\b(40[0-9a-f]{6})\b", log.split(">>>stack>>>")[-1])))
if addrs and os.path.exists(elf):
    out = subprocess.run([a2l, "-e", elf, "-f", "-C", "-i"] + addrs, capture_output=True, text=True).stdout.splitlines()
    for a, i in zip(addrs, range(0, len(out), 2)):
        print(a, out[i], out[i + 1] if i + 1 < len(out) else "")
