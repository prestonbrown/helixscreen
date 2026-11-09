# SPDX-License-Identifier: GPL-3.0-or-later
"""Mini Stealthburner: a pear-shaped shell, narrow at the top, on a dark extruder."""
from ..head import Drawing, HeadDef, mirror

BODY, REAR, DARK = "#cf2b2b", "#2e2f32", "#141517"
SHELL = mirror(
    [(0.0, 0.13), (0.07, 0.13), (0.1, 0.03), (0.24, 0.03), (0.3, 0.14), (0.29, 0.34), (0.21, 0.55),
     (0.2, 0.75), (0.15, 0.9), (0.0, 0.95)]
)
TIP = (0.0, 0.1, -0.07)


def draw_art(scene, tier):
    d = Drawing(scene, tier)
    d.solid(mirror([(0.0, 0.08), (0.18, 0.08), (0.27, 0.45), (0.2, 0.92), (0.0, 0.96)]), 0.18, 0.46, REAR)
    d.tube(0.02, 0.28, 0.93, 1.0, 0.075, "#8a8b8f")
    d.nozzle(TIP[0], TIP[1], 0.09, block="#8e8f92")
    d.solid(SHELL, 0.0, 0.2, BODY, bevel=0.04)
    d.facet([(0.0, 0.0, 0.93), (-0.14, 0.0, 0.88), (-0.17, 0.0, 0.72), (-0.05, 0.0, 0.78)], "#e6443f")
    d.facet([(0.0, 0.0, 0.93), (0.14, 0.0, 0.88), (0.17, 0.0, 0.72), (0.05, 0.0, 0.78)], "#ad2424")
    d.fan(0.0, 0.3, 0.2, -0.002)
    if d.detail:
        for x, z in ((-0.15, 0.56), (0.15, 0.56)):
            d.disc(x, z, 0.02, -0.001, DARK, sides=12)


HEAD = HeadDef("mini_stealthburner", "MINI_STEALTHBURNER", draw_art, TIP)
