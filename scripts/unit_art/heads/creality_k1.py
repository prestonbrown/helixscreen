# SPDX-License-Identifier: GPL-3.0-or-later
"""Creality K1: a satin grey shell, wide at the shoulders, a low fan, and open side vents."""
from ..head import Drawing, HeadDef, rounded

SHELL, SIDE, DARK = "#a7a9ad", "#8f9195", "#17181a"
FACE = rounded([(-0.26, 0.05), (0.26, 0.05), (0.34, 0.35), (0.34, 0.86), (-0.34, 0.86), (-0.34, 0.35)], 0.09)
TIP = (0.0, 0.1, -0.07)


def draw_art(scene, tier):
    d = Drawing(scene, tier)
    # The black carriage block and cable port above the shell.
    d.solid([(-0.16, 0.84), (0.16, 0.84), (0.16, 0.98), (-0.16, 0.98)], 0.2, 0.42, "#1c1d20")
    d.tube(0.0, 0.32, 0.96, 1.0, 0.04, "#d9dadc")
    d.nozzle(TIP[0], TIP[1], 0.07, block="#8e8f92")
    d.solid(FACE, 0.0, 0.5, SHELL, bevel=0.035)
    d.disc(0.0, 0.3, 0.17, -0.002, "#c3c4c7")
    d.fan(0.0, 0.3, 0.13, -0.004)
    # Vent cut-outs along the right flank.
    for z0, z1 in ((0.62, 0.78), (0.3, 0.54), (0.14, 0.24)):
        d.facet([(0.34, 0.12, z0), (0.34, 0.26, z0), (0.34, 0.26, z1), (0.34, 0.12, z1)], DARK, amb=0.9, dif=0.2, spec=0.0)
    if d.detail:
        d.facet([(0.34, 0.32, 0.4), (0.34, 0.42, 0.4), (0.34, 0.42, 0.74), (0.34, 0.32, 0.74)], DARK, amb=0.9, dif=0.2, spec=0.0)


HEAD = HeadDef("creality_k1", "CREALITY_K1", draw_art, TIP)
