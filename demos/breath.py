#!/usr/bin/env python3
"""Calm full-panel breathing via the /img API.  python3 breath.py [seconds] [host]"""
import math, socket, sys, time, urllib.request

N = 16 * 8          # two 8x8 panels
PEAK = (18, 11, 0)  # warm yellow; 128 LEDs x (18+11) stays just under the 300mA cap in pico/power.h
PERIOD = 5.0        # seconds per breath
FLOOR = 0.04        # never fully off: avoids the coarse steps at the bottom of 8-bit

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 30
host = socket.gethostbyname(sys.argv[2] if len(sys.argv) > 2 else 'pico-esp.local')
t0 = time.monotonic()
while (t := time.monotonic() - t0) < secs:
    level = (1 - math.cos(2 * math.pi * t / PERIOD)) / 2          # smooth 0..1..0
    level = FLOOR + (1 - FLOOR) * level ** 2.2                     # gamma: eyes see linear steps as fast at the bottom
    px = '%02x%02x%02x' % tuple(round(c * level) for c in PEAK)
    try:
        urllib.request.urlopen(f'http://{host}/img?d={px * N}', timeout=1).read()
    except OSError:
        pass
urllib.request.urlopen(f'http://{host}/img?d={"000000" * N}', timeout=1).read()
