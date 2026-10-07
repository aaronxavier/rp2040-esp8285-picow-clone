#!/usr/bin/env python3
"""Calm full-panel breathing via the /img API.  python3 breath.py [seconds] [host]"""
import math, socket, sys, time, urllib.request

N = 16 * 8          # two 8x8 panels
PEAK = (255, 170, 0)  # warm yellow, perceptual; the Pico applies gamma and the global brightness
PERIOD = 5.0        # seconds per breath
FLOOR = 0.15         # never fully off: at low brightness the bottom of the curve rounds to 0

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 30
host = socket.gethostbyname(sys.argv[2] if len(sys.argv) > 2 else 'pico-esp.local')
t0 = time.monotonic()
while (t := time.monotonic() - t0) < secs:
    level = FLOOR + (1 - FLOOR) * (1 - math.cos(2 * math.pi * t / PERIOD)) / 2  # smooth, perceptually even
    px = '%02x%02x%02x' % tuple(round(c * level) for c in PEAK)
    try:
        urllib.request.urlopen(f'http://{host}/img?d={px * N}', timeout=1).read()
    except OSError:
        pass
urllib.request.urlopen(f'http://{host}/img?d={"000000" * N}', timeout=1).read()
