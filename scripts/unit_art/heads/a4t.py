# SPDX-License-Identifier: GPL-3.0-or-later
"""Triangle-Lab A4T: a black fan body, a gold band, and a dark extruder with a gold lever."""
from ..head import Drawing, HeadDef, mirror, rounded

BLACK, GOLD, DARK = "#3e3f44", "#b8a23a", "#35363a"
BODY = mirror([(0.0, 0.1), (0.1, 0.1), (0.14, 0.04), (0.3, 0.04), (0.34, 0.14), (0.34, 0.5), (0.3, 0.6), (0.0, 0.6)])
BAND = mirror([(0.0, 0.6), (0.2, 0.6), (0.26, 0.62), (0.26, 0.66), (0.0, 0.66)])
TIP = (0.0, 0.1, -0.07)


def draw_art(scene, tier):
    d = Drawing(scene, tier)
    # Rear extruder and its motor.
    d.tube(0.1, 0.33, 0.66, 0.92, 0.17, DARK)
    d.solid(rounded([(-0.18, 0.66), (0.18, 0.66), (0.18, 0.9), (0.1, 1.0), (-0.12, 1.0), (-0.18, 0.9)], 0.03), 0.06, 0.32, DARK)
    d.nozzle(TIP[0], TIP[1], 0.08, block="#b76a3f")
    d.solid(BODY, 0.0, 0.42, BLACK, bevel=0.03)
    d.solid(BAND, 0.02, 0.36, GOLD, bevel=0.012)
    d.face([(-0.13, 0.66), (0.13, 0.66), (0.08, 0.58), (-0.08, 0.58)], 0.019, GOLD)
    d.fan(0.0, 0.36, 0.17, -0.002)
    # The gold idler lever, leaning right across the extruder face.
    d.solid([(-0.02, 0.7), (0.16, 0.84), (0.2, 0.98), (0.13, 1.0), (0.09, 0.9), (-0.06, 0.78)], 0.02, 0.08, GOLD, bevel=0.012)
    d.disc(-0.07, 0.85, 0.05, 0.05, "#141517", sides=16)
    if d.detail:
        for x in (-0.22, 0.22):
            d.disc(x, 0.16, 0.022, -0.001, "#6b6c70", sides=12)
        d.disc(0.04, 0.72, 0.02, 0.018, "#6b6c70", sides=10)


HEAD = HeadDef("a4t", "A4T", draw_art, TIP)
