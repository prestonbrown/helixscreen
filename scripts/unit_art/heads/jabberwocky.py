# SPDX-License-Identifier: GPL-3.0-or-later
"""Jabberwocky: an orange box with a blue fan ring, a grey core and a dark top bracket."""
from ..head import Drawing, HeadDef, mirror, rounded

ORANGE, GREY, BLUE, DARK, GLOW = "#e8892b", "#a9acb0", "#2f8fd0", "#1b1c1f", "#a9dfae"
BOX = mirror([(0.0, 0.05), (0.2, 0.05), (0.27, 0.13), (0.27, 0.47), (0.2, 0.56), (0.0, 0.56)])
TIP = (0.0, 0.1, -0.07)


def draw_art(scene, tier):
    d = Drawing(scene, tier)
    # The rear gearbox and cable bracket.
    d.tube(-0.12, 0.32, 0.56, 0.78, 0.17, ORANGE)
    d.solid([(-0.2, 0.74), (-0.08, 0.74), (-0.08, 1.0), (-0.2, 1.0)], 0.22, 0.3, DARK)
    d.tube(0.08, 0.14, 0.72, 0.98, 0.012, "#2f8f8a")
    d.nozzle(TIP[0], TIP[1], 0.07, block="#8e8f92")
    d.solid(rounded([(-0.17, 0.56), (0.2, 0.56), (0.2, 0.74), (0.1, 0.86), (-0.06, 0.86), (-0.17, 0.76)], 0.03), 0.03, 0.34, GREY, bevel=0.015)
    d.face(rounded([(-0.05, 0.6), (0.06, 0.6), (0.06, 0.74), (0.0, 0.8), (-0.05, 0.74)], 0.02), 0.012, GLOW)
    d.solid(BOX, 0.0, 0.36, ORANGE, bevel=0.03)
    d.disc(-0.07, 0.3, 0.15, -0.002, BLUE, amb=0.8, dif=0.4)
    d.fan(-0.07, 0.3, 0.11, -0.004)
    # The blue lever down the left edge.
    d.solid([(-0.31, 0.46), (-0.27, 0.46), (-0.12, 0.74), (-0.17, 0.78)], 0.0, 0.08, BLUE, bevel=0.01)
    # The dark blower mouth on the right face.
    d.face(rounded([(0.2, 0.2), (0.26, 0.2), (0.26, 0.42), (0.2, 0.42)], 0.02), -0.002, DARK, amb=0.9, dif=0.2, spec=0.0)


HEAD = HeadDef("jabberwocky", "JABBERWOCKY", draw_art, TIP)
