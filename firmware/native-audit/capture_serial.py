#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Raw serial capture for the K-Touch audit builds. idf.py monitor is not
# usable over the CH340 (see docs/devel/plans/ESP32_NATIVE_AUDIT.md traps) — this pulses RTS
# for a programmatic reset, then streams the boot log for --seconds.
#
# Usage: capture_serial.py [--port /dev/ttyUSB0] [--seconds 40] [--out file]

import argparse
import sys
import time

import serial

ap = argparse.ArgumentParser()
ap.add_argument("--port", default="/dev/ttyUSB0")
ap.add_argument("--baud", type=int, default=460800)
ap.add_argument("--seconds", type=float, default=40)
ap.add_argument("--out", default="/dev/stdout")
ap.add_argument("--no-reset", action="store_true")
args = ap.parse_args()

s = serial.Serial(args.port, args.baud, timeout=0.5)
if not args.no_reset:
    # RTS pulse = EN reset on the CH340 wiring (DTR held low = normal boot)
    s.dtr = False
    s.rts = True
    time.sleep(0.1)
    s.rts = False
s.reset_input_buffer()

deadline = time.time() + args.seconds
out = open(args.out, "wb") if args.out != "/dev/stdout" else sys.stdout.buffer
try:
    while time.time() < deadline:
        data = s.read(4096)
        if data:
            out.write(data)
            out.flush()
finally:
    if out is not sys.stdout.buffer:
        out.close()
    s.close()
