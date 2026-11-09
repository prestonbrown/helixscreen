# SPDX-License-Identifier: GPL-3.0-or-later
"""AntHead: a tall purple shell around a grey centre plate, with a hex-bolt extruder on top."""
from ..head import Drawing, HeadDef, mirror

BODY, PLATE, DARK, GREY = "#a92bb0", "#83868a", "#141517", "#6f7276"
SHELL = mirror(
    [(0.0, 0.02), (0.1, 0.02), (0.15, 0.12), (0.2, 0.1), (0.25, 0.2), (0.25, 0.46), (0.17, 0.62), (0.15, 0.66), (0.0, 0.66)]
)
TIP = (0.0, 0.1, -0.07)


def draw_art(scene, tier):
    d = Drawing(scene, tier)
    d.solid(mirror([(0.0, 0.62), (0.16, 0.62), (0.2, 0.75), (0.15, 0.93), (0.0, 1.0)]), 0.08, 0.36, BODY)
    d.nozzle(TIP[0], TIP[1], 0.09, block="#8e8f92")
    d.solid(SHELL, 0.0, 0.36, BODY, bevel=0.03)
    plate = mirror([(0.0, 0.08), (0.14, 0.14), (0.17, 0.26), (0.13, 0.55), (0.0, 0.57)])
    d.face(plate, -0.002, PLATE)
    d.disc(0.0, 0.3, 0.14, -0.004, "#8a2d8f", sides=6, amb=0.8, dif=0.4)
    d.fan(0.0, 0.3, 0.09, -0.006)
    # The hex bolt and the grey idler arm with its thumb screw.
    d.solid([(0.05, 0.72), (0.24, 0.84), (0.27, 0.92), (0.18, 0.9), (0.05, 0.8)], 0.03, 0.09, GREY, bevel=0.01)
    d.tube(0.29, 0.06, 0.88, 0.94, 0.03, "#2a2b2e")
    d.disc(-0.05, 0.82, 0.075, 0.0, GREY, sides=6, amb=0.8, dif=0.5)
    d.disc(-0.05, 0.82, 0.04, -0.002, "#55585c", sides=6, amb=0.8, dif=0.5)
    if d.detail:
        d.disc(0.13, 0.77, 0.018, 0.02, DARK, sides=10)


HEAD = HeadDef("anthead", "ANTHEAD", draw_art, TIP)
