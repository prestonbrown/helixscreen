# SPDX-License-Identifier: GPL-3.0-or-later
"""Dragonburner: a red extruder stack on a blue fan body with black side shrouds."""
from ..head import Drawing, HeadDef, mirror, rounded

RED, BLUE, BLACK, GREEN = "#c7312c", "#2f6fb6", "#1c1d20", "#3b8c44"
BODY = mirror([(0.0, 0.1), (0.2, 0.1), (0.24, 0.16), (0.24, 0.56), (0.2, 0.62), (0.0, 0.62)])
TIP = (0.0, 0.1, -0.07)


def draw_art(scene, tier):
    d = Drawing(scene, tier)
    # Extruder stack, rear plate first.
    d.solid(rounded([(-0.16, 0.62), (0.2, 0.62), (0.2, 0.86), (0.08, 1.0), (-0.1, 1.0), (-0.16, 0.9)], 0.03), 0.1, 0.36, RED)
    d.tube(0.2, 0.2, 0.8, 0.86, 0.05, "#c9b18a")
    d.solid([(0.14, 0.8), (0.3, 0.8), (0.3, 0.87), (0.14, 0.87)], 0.02, 0.1, "#b0b2b6")
    d.disc(0.0, 0.92, 0.06, 0.09, "#d7a53f", sides=16)
    d.nozzle(TIP[0], TIP[1], 0.09, block="#8e8f92")
    d.solid(mirror([(0.0, 0.03), (0.2, 0.03), (0.26, 0.1), (0.26, 0.16), (0.0, 0.16)]), 0.0, 0.4, BLUE, bevel=0.02)
    # Side shrouds and the blower on the right.
    d.solid([(0.24, 0.16), (0.33, 0.2), (0.33, 0.56), (0.24, 0.6)], 0.04, 0.4, BLACK)
    d.solid([(-0.24, 0.16), (-0.33, 0.2), (-0.33, 0.56), (-0.24, 0.6)], 0.04, 0.4, BLACK)
    d.solid(BODY, 0.0, 0.42, BLUE, bevel=0.025)
    d.facet([(0.33, 0.1, 0.42), (0.33, 0.3, 0.42), (0.33, 0.3, 0.32), (0.33, 0.1, 0.32)], "#282a2e")
    d.fan(0.0, 0.35, 0.18, -0.002)
    # The red top plate and its hex-head idler knob.
    d.solid(mirror([(0.0, 0.62), (0.22, 0.62), (0.22, 0.7), (0.0, 0.7)]), -0.01, 0.14, RED, bevel=0.015)
    if d.detail:
        d.solid([(0.2, 0.62), (0.3, 0.62), (0.3, 0.68), (0.2, 0.68)], 0.02, 0.2, GREEN)
    d.disc(-0.08, 0.84, 0.1, -0.002, BLACK, sides=6, amb=0.9, dif=0.4)
    d.disc(-0.08, 0.84, 0.05, -0.004, "#333539", sides=6, amb=0.9, dif=0.4)
    d.solid(rounded([(-0.16, 0.7), (0.2, 0.7), (0.2, 0.92), (0.05, 0.98), (-0.12, 0.98), (-0.16, 0.9)], 0.03), 0.03, 0.1, RED, bevel=0.01)
    d.disc(-0.08, 0.84, 0.1, 0.028, BLACK, sides=6, amb=0.9, dif=0.4)
    d.disc(-0.08, 0.84, 0.05, 0.026, "#333539", sides=6, amb=0.9, dif=0.4)


HEAD = HeadDef("dragonburner", "DRAGONBURNER", draw_art, TIP)
