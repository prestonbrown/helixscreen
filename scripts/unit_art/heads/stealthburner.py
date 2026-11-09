# SPDX-License-Identifier: GPL-3.0-or-later
"""Voron Stealthburner: a keyhole-shaped faceted shell over a dark rear cowl."""
from ..head import Drawing, HeadDef, mirror

BODY, REAR, SEAM, HOLE = "#c92a2a", "#3a3b3e", "#b9babd", "#141517"
SHELL = mirror(
    [(0.0, 0.14), (0.06, 0.14), (0.09, 0.03), (0.23, 0.03), (0.28, 0.1), (0.27, 0.3), (0.2, 0.48),
     (0.21, 0.62), (0.27, 0.74), (0.27, 0.87), (0.19, 0.98), (0.0, 1.0)]
)
COWL = mirror([(0.0, 0.1), (0.18, 0.1), (0.23, 0.4), (0.27, 0.72), (0.25, 0.92), (0.14, 1.0), (0.0, 1.02)])
TIP = (0.0, 0.1, -0.07)


def draw_art(scene, tier):
    d = Drawing(scene, tier)
    d.solid(COWL, 0.2, 0.52, REAR)
    d.nozzle(TIP[0], TIP[1], 0.09, block="#8e8f92")
    d.solid(SHELL, 0.16, 0.2, SEAM)
    # The latch horn at the upper left.
    d.solid([(-0.2, 0.86), (-0.35, 1.0), (-0.16, 0.98)], 0.1, 0.22, BODY)
    d.solid(SHELL, 0.0, 0.16, BODY, bevel=0.035)
    d.facet([(-0.2, 0.0, 0.98), (-0.27, 0.0, 0.87), (-0.13, 0.0, 0.9), (-0.08, 0.0, 0.96)], "#e2453f")
    d.facet([(0.19, 0.0, 0.98), (0.27, 0.0, 0.87), (0.13, 0.0, 0.9), (0.08, 0.0, 0.96)], "#ae2222")
    d.face([(0.14 * c, 0.78 + 0.14 * s) for c, s in [(1, 0), (0.5, 0.87), (-0.5, 0.87), (-1, 0), (-0.5, -0.87), (0.5, -0.87)]], -0.001, HOLE, amb=0.9, dif=0.2, spec=0.0)
    d.disc(0.0, 0.78, 0.075, -0.003, "#26272a")
    d.fan(0.0, 0.3, 0.19, -0.002)
    if d.detail:
        for x, z in ((-0.17, 0.55), (0.17, 0.55), (-0.2, 0.11), (0.2, 0.11)):
            d.disc(x, z, 0.018, -0.001, HOLE, sides=12)


HEAD = HeadDef("stealthburner", "STEALTHBURNER", draw_art, TIP)
