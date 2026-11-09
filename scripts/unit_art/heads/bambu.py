# SPDX-License-Identifier: GPL-3.0-or-later
"""Bambu Lab: a white rounded box with a recessed panel, a fan, and a dark notched skirt."""
from ..head import Drawing, HeadDef, mirror, rounded

WHITE, PANEL, DARK = "#e4e4e2", "#d5d5d3", "#26272a"
BODY = rounded([(-0.26, 0.2), (0.26, 0.2), (0.26, 1.0), (-0.26, 1.0)], 0.07)
SKIRT = mirror([(0.0, 0.05), (0.07, 0.05), (0.07, 0.11), (0.13, 0.11), (0.13, 0.03), (0.22, 0.03), (0.26, 0.1), (0.26, 0.22), (0.0, 0.22)])
TIP = (0.0, 0.1, -0.07)


def draw_art(scene, tier):
    d = Drawing(scene, tier)
    d.nozzle(TIP[0], TIP[1], 0.07, block="#8e8f92")
    d.solid(SKIRT, 0.0, 0.4, DARK, bevel=0.02)
    d.solid(BODY, 0.0, 0.4, WHITE, bevel=0.03)
    d.face(rounded([(-0.18, 0.26), (0.18, 0.26), (0.18, 0.86), (-0.18, 0.86)], 0.05), -0.001, PANEL)
    d.disc(0.0, 0.47, 0.16, -0.002, "#bdbdbb")
    d.fan(0.0, 0.47, 0.13, -0.004)


HEAD = HeadDef("bambu", "BAMBU", draw_art, TIP)
